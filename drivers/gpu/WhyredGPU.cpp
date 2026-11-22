#if defined(ARM64_BOARD_CONFIG_WHYRED)

/*
 * WhyredGPU: the Adreno 509 for programs to use.
 *
 * whyred_gpu.c wakes the GPU and starts the command processor (the ladder up to step 12, boot-arg
 * whyredgpu=30); when the ring is up it calls WhyredGPUStart() and this service appears. A program
 * opens it with IOServiceOpen() and speaks four requests (selectors of IOConnectCallStructMethod):
 *
 *   0 kWhyredInfo     -> struct whyred_info: version, whether the GPU is ready, how many streams went in, last fence
 *   1 kWhyredBufNew   <- struct whyred_buf_new {size}, -> {handle, address}: a buffer of physically contiguous memory,
 *                     zeroed, write-combined; address is what the GPU calls it (the GPU's translation of this
 *                     driver is the identity, so it is the physical address). The program maps it with
 *                     IOConnectMapMemory64(handle as the memory type).
 *   2 kWhyredBufFree  <- struct whyred_buf_free {handle}
 *   3 kWhyredSubmit   <- an array of 32-bit PM4 words, -> struct whyred_submit_out {fence, status}: the words go into the
 *                     ring followed by a cache flush and a fence write, and the call returns when the GPU has
 *                     reached the fence (it is synchronous: one stream at a time).
 *
 * Nothing here protects the system from a program that names addresses it does not own: this is the bring-up
 * interface (one trusted client, a root test tool or the Metal plug-in), not a security boundary.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <mach/vm_param.h>
#include <sys/systm.h>

extern "C" {
int whyred_gpu_service_ready(void);
uint32_t whyred_gpu_service_submit(const uint32_t * dwords, uint32_t count, int * status);
void WhyredGPUStart(void);
void WhyredGPUSelfTest(void);
}

enum {
	kWhyredInfo        = 0,
	kWhyredBufNew      = 1,
	kWhyredBufFree     = 2,
	kWhyredSubmit      = 3,
};

#define WHYRED_BUFFERS     64
#define WHYRED_BUFFER_MAX  (16ull << 20)
#define WHYRED_SUBMIT_MAX  16384           /* bytes of PM4 words in one stream */

struct whyred_info {
	uint32_t version, ready, submits, last_fence;
};
struct whyred_buf_new {
	uint64_t size;
	uint32_t handle, pad;
	uint64_t address;
};
struct whyred_buf_free {
	uint32_t handle, pad;
};
struct whyred_submit_out {
	uint32_t fence, status;
};

static IOLock * gWhyredLock;
static uint32_t gWhyredSubmits, gWhyredLast;

class WhyredGPU : public IOService
{
	OSDeclareDefaultStructors(WhyredGPU);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(WhyredGPU, IOService);

bool
WhyredGPU::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "WhyredGPUUserClient");
	setName("WhyredGPU");
	registerService();
	return true;
}

void
WhyredGPUStart(void)
{
	IOService * provider = IOService::getPlatform();
	WhyredGPU * gpu;

	if (gWhyredLock == NULL) {
		gWhyredLock = IOLockAlloc();
	}
	gpu = OSTypeAlloc(WhyredGPU);
	if (gpu == NULL || provider == NULL || !gpu->init() || !gpu->attach(provider)) {
		OSSafeReleaseNULL(gpu);
		IOLog("WhyredGPU: cannot create the service\n");
		return;
	}
	if (!gpu->start(provider)) {
		gpu->detach(provider);
		IOLog("WhyredGPU: the service did not start\n");
	} else {
		IOLog("WhyredGPU: service started\n");
	}
	gpu->release();
}

class WhyredGPUUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(WhyredGPUUserClient);

	task_t                          fTask;
	IOBufferMemoryDescriptor *      fMemory[WHYRED_BUFFERS];   /* handle n is fMemory[n - 1] */
	uint64_t                        fAddress[WHYRED_BUFFERS];

	IOReturn bufferNew(struct whyred_buf_new * request);
	IOReturn bufferFree(uint32_t handle);
	IOReturn submit(IOExternalMethodArguments * args, struct whyred_submit_out * out);

public:
	bool selfTest(void);
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(WhyredGPUUserClient, IOUserClient);

bool
WhyredGPUUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

IOReturn
WhyredGPUUserClient::clientClose(void)
{
	IOLockLock(gWhyredLock);
	for (uint32_t i = 0; i < WHYRED_BUFFERS; i++) {
		if (fMemory[i] != NULL) {
			fMemory[i]->release();
			fMemory[i] = NULL;
		}
	}
	IOLockUnlock(gWhyredLock);
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

IOReturn
WhyredGPUUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(gWhyredLock);
	if (type >= 1 && type <= WHYRED_BUFFERS && fMemory[type - 1] != NULL) {
		fMemory[type - 1]->retain();
		*memory = fMemory[type - 1];
		*options = 0;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gWhyredLock);
	return ret;
}

IOReturn
WhyredGPUUserClient::bufferNew(struct whyred_buf_new * request)
{
	IOBufferMemoryDescriptor * memory;
	uint64_t size = round_page_64(request->size);
	uint32_t slot = WHYRED_BUFFERS;

	if (request->size == 0 || size > WHYRED_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	/* below 4 GB and in one piece: the GPU's page table is the identity of the first four gigabytes */
	memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapWriteCombineCache,
	    size, 0x00000000FFFFF000ULL);
	if (memory == NULL) {
		IOLog("WhyredGPU: no contiguous memory for a buffer of %llu bytes\n", size);
		return kIOReturnNoMemory;
	}
	IOLockLock(gWhyredLock);
	for (uint32_t i = 0; i < WHYRED_BUFFERS; i++) {
		if (fMemory[i] == NULL) {
			slot = i;
			break;
		}
	}
	if (slot == WHYRED_BUFFERS) {
		IOLockUnlock(gWhyredLock);
		memory->release();
		return kIOReturnNoResources;
	}
	fMemory[slot] = memory;
	fAddress[slot] = memory->getPhysicalSegment(0, NULL, kIOMemoryMapperNone);
	request->handle = slot + 1;
	request->address = fAddress[slot];
	request->size = size;
	IOLockUnlock(gWhyredLock);
	return kIOReturnSuccess;
}

IOReturn
WhyredGPUUserClient::bufferFree(uint32_t handle)
{
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(gWhyredLock);
	if (handle >= 1 && handle <= WHYRED_BUFFERS && fMemory[handle - 1] != NULL) {
		fMemory[handle - 1]->release();
		fMemory[handle - 1] = NULL;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gWhyredLock);
	return ret;
}

/* The stream comes inline (up to 4 KB) or as a memory descriptor (larger); either way it is copied before use. */
IOReturn
WhyredGPUUserClient::submit(IOExternalMethodArguments * args, struct whyred_submit_out * out)
{
	uint32_t * words;
	size_t size;
	IOReturn ret = kIOReturnSuccess;
	int status = 0;

	if (args->structureInputDescriptor != NULL) {
		size = (size_t)args->structureInputDescriptor->getLength();
	} else {
		size = args->structureInputSize;
	}
	if (size == 0 || size > WHYRED_SUBMIT_MAX || (size & 3) != 0) {
		return kIOReturnBadArgument;
	}
	words = (uint32_t *)IOMalloc(size);
	if (words == NULL) {
		return kIOReturnNoMemory;
	}
	if (args->structureInputDescriptor != NULL) {
		if (args->structureInputDescriptor->readBytes(0, words, size) != size) {
			IOFree(words, size);
			return kIOReturnVMError;
		}
	} else {
		memcpy(words, args->structureInput, size);
	}
	IOLockLock(gWhyredLock);
	out->fence = whyred_gpu_service_submit(words, (uint32_t)(size / 4), &status);
	if (out->fence != 0) {
		gWhyredSubmits++;
		gWhyredLast = out->fence;
	}
	IOLockUnlock(gWhyredLock);
	out->status = (uint32_t)status;
	if (status != 0) {
		ret = kIOReturnIOError;
	}
	IOFree(words, size);
	return ret;
}

IOReturn
WhyredGPUUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	union {
		struct whyred_info info;
		struct whyred_buf_new bufNew;
		struct whyred_buf_free bufFree;
		struct whyred_submit_out out;
	} r;
	size_t size = args->structureInputSize;
	IOReturn ret = kIOReturnSuccess;
	size_t outSize = 0;

	bzero(&r, sizeof(r));
	switch (selector) {
	case kWhyredInfo:
		r.info.version = 1;
		r.info.ready = whyred_gpu_service_ready() ? 1 : 0;
		r.info.submits = gWhyredSubmits;
		r.info.last_fence = gWhyredLast;
		outSize = sizeof(r.info);
		break;
	case kWhyredBufNew:
		if (args->structureInput == NULL || size < sizeof(uint64_t)) {
			return kIOReturnBadArgument;
		}
		memcpy(&r.bufNew, args->structureInput, size < sizeof(r.bufNew) ? size : sizeof(r.bufNew));
		ret = bufferNew(&r.bufNew);
		outSize = sizeof(r.bufNew);
		break;
	case kWhyredBufFree:
		if (args->structureInput == NULL || size < sizeof(uint32_t)) {
			return kIOReturnBadArgument;
		}
		memcpy(&r.bufFree, args->structureInput, size < sizeof(r.bufFree) ? size : sizeof(r.bufFree));
		ret = bufferFree(r.bufFree.handle);
		break;
	case kWhyredSubmit:
		if (!whyred_gpu_service_ready()) {
			return kIOReturnNotReady;
		}
		ret = submit(args, &r.out);
		outSize = sizeof(r.out);
		break;
	default:
		IOLog("WhyredGPU: request %u is not known here\n", selector);
		return kIOReturnUnsupported;
	}
	if (args->structureOutput != NULL && outSize != 0) {
		if (args->structureOutputSize > outSize) {
			args->structureOutputSize = (uint32_t)outSize;
		}
		memcpy(args->structureOutput, &r, args->structureOutputSize);
	}
	return ret;
}

/* PM4 type 7 header: odd parity of the count and of the opcode, as the CP wants it. */
static uint32_t
whyred_parity(uint32_t v)
{
	v ^= v >> 16;
	v ^= v >> 8;
	v ^= v >> 4;
	return (0x9669u >> (v & 0xf)) & 1u;
}

static uint32_t
whyred_pkt7(uint32_t opcode, uint32_t count)
{
	return 0x70000000u | (count & 0x3fff) | (whyred_parity(count) << 15) | ((opcode & 0x7f) << 16) | (whyred_parity(opcode) << 23);
}

/*
 * What a program would do, from inside the kernel: open (initWithTask), ask for the state, make a buffer, have the
 * GPU write two words into it with CP_MEM_WRITE through a submitted stream, read them back, give the buffer back.
 */
bool
WhyredGPUUserClient::selfTest(void)
{
	IOExternalMethodArguments args;
	struct whyred_info info;
	struct whyred_buf_new bn;
	struct whyred_buf_free bf;
	struct whyred_submit_out out;
	uint32_t stream[8];
	volatile uint32_t * words;
	IOReturn ret;
	bool ok = false;

	bzero(&args, sizeof(args));
	bzero(&info, sizeof(info));
	args.structureOutput = &info;
	args.structureOutputSize = sizeof(info);
	ret = externalMethod(kWhyredInfo, &args, NULL, NULL, NULL);
	IOLog("WhyredGPU: selftest info: ret %x version %u ready %u submits %u last %u\n", ret, info.version, info.ready, info.submits, info.last_fence);

	bzero(&bn, sizeof(bn));
	bn.size = 4096;
	bzero(&args, sizeof(args));
	args.structureInput = &bn;
	args.structureInputSize = sizeof(bn);
	args.structureOutput = &bn;
	args.structureOutputSize = sizeof(bn);
	ret = externalMethod(kWhyredBufNew, &args, NULL, NULL, NULL);
	IOLog("WhyredGPU: selftest buffer: ret %x handle %u address %llx size %llu\n", ret, bn.handle, bn.address, bn.size);
	if (ret != kIOReturnSuccess || bn.handle == 0) {
		return false;
	}
	words = (volatile uint32_t *)fMemory[bn.handle - 1]->getBytesNoCopy();

	stream[0] = whyred_pkt7(61, 3);                    /* CP_MEM_WRITE: address, then the words */
	stream[1] = (uint32_t)bn.address;
	stream[2] = (uint32_t)(bn.address >> 32);
	stream[3] = 0x5eed1234u;
	stream[4] = whyred_pkt7(61, 3);
	stream[5] = (uint32_t)(bn.address + 4);
	stream[6] = (uint32_t)((bn.address + 4) >> 32);
	stream[7] = 0x600dcafeu;
	bzero(&args, sizeof(args));
	bzero(&out, sizeof(out));
	args.structureInput = stream;
	args.structureInputSize = sizeof(stream);
	args.structureOutput = &out;
	args.structureOutputSize = sizeof(out);
	ret = externalMethod(kWhyredSubmit, &args, NULL, NULL, NULL);
	IOLog("WhyredGPU: selftest submit: ret %x fence %u status %u, buffer words %08x %08x (want 5eed1234 600dcafe)\n",
	    ret, out.fence, out.status, words[0], words[1]);
	ok = ret == kIOReturnSuccess && words[0] == 0x5eed1234u && words[1] == 0x600dcafeu;

	bzero(&bf, sizeof(bf));
	bf.handle = bn.handle;
	bzero(&args, sizeof(args));
	args.structureInput = &bf;
	args.structureInputSize = sizeof(bf);
	ret = externalMethod(kWhyredBufFree, &args, NULL, NULL, NULL);
	IOLog("WhyredGPU: selftest free: ret %x; %s\n", ret, ok ? "PASSED" : "FAILED");
	return ok;
}

void
WhyredGPUSelfTest(void)
{
	WhyredGPUUserClient * client = OSTypeAlloc(WhyredGPUUserClient);

	if (client == NULL || !client->init() || !client->initWithTask(kernel_task, NULL, 0, NULL)) {
		IOLog("WhyredGPU: selftest: no client\n");
		OSSafeReleaseNULL(client);
		return;
	}
	client->selfTest();
	client->clientClose();
	client->release();
}

#else /* !ARM64_BOARD_CONFIG_WHYRED */

extern "C" int whyred_gpu_service_not_built;

#endif /* ARM64_BOARD_CONFIG_WHYRED */
