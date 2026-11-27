#if defined(ARM64_BOARD_CONFIG_WHYRED)

/*
 * WhyredGPU: the Adreno 509 for programs to use.
 *
 * whyred_gpu.c wakes the GPU and starts the command processor (the ladder up to step 12, boot-arg
 * whyredgpu=30); when the ring is up it calls WhyredGPUStart() and this service appears. A program
 * opens it with IOServiceOpen() and speaks six requests (selectors of IOConnectCallStructMethod):
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
 *   4 kWhyredSubmitBuf <- a stream already in a client buffer: {handle, offset, dwords}; same fence.
 *   5 kWhyredMetal    <- {on}: put MetalPluginName/MetalPluginClassName on IOAcceleratorES (or remove them).
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
#include <IOKit/IOSubMemoryDescriptor.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
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
	kWhyredSubmitBuf   = 4,
	kWhyredMetal       = 5,
};

#define WHYRED_BUFFERS     256
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
struct whyred_submit_buf {
	uint32_t handle, offset, dwords, pad;
};

static IOLock * gWhyredLock;
static uint32_t gWhyredSubmits, gWhyredLast;
static uint32_t gWhyredVerbose;
static IOReturn whyredMetalSet(uint32_t on);

/*
 * The pool: physically contiguous memory below 4 GB is nearly all gone a few minutes after boot (8 MB and 2 MB requests fail), and
 * the GPU needs buffers in one piece. So the service takes one big block while the system is still young (WhyredGPUStart) and
 * hands pieces of it out, a page at a time; when there is no pool a buffer is its own allocation, as before.
 */
#define WHYRED_POOL_PAGE   4096ull
static IOBufferMemoryDescriptor * gPool;
static uint64_t gPoolPhys, gPoolSize;
static uint8_t * gPoolUsed;                     /* one byte per page, under gWhyredLock */
static uint32_t gPoolPages;
static int gPoolTried;

static void
whyredPoolInit(void)
{
	static const uint64_t sizes[] = { 128ull << 20, 64ull << 20, 32ull << 20, 16ull << 20, 8ull << 20, 4ull << 20 };

	for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]) && gPool == NULL; i++) {
		printf("WhyredGPU: pool of %llu MB: trying\n", sizes[i] >> 20);
		IOBufferMemoryDescriptor * m = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		    kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapWriteCombineCache, sizes[i], 0x00000000FFFFF000ULL);

		IOLog("WhyredGPU: pool of %llu MB: %s\n", sizes[i] >> 20, m != NULL ? "got it" : "no contiguous memory");
		if (m != NULL) {
			gPoolPages = (uint32_t)(sizes[i] / WHYRED_POOL_PAGE);
			gPoolUsed = (uint8_t *)IOMalloc(gPoolPages);
			if (gPoolUsed == NULL) {
				m->release();
				return;
			}
			bzero(gPoolUsed, gPoolPages);
			gPoolPhys = m->getPhysicalSegment(0, NULL, kIOMemoryMapperNone);
			gPoolSize = sizes[i];
			gPool = m;
			IOLog("WhyredGPU: pool: pa %llx size %llu MB\n", gPoolPhys, gPoolSize >> 20);
		}
	}
	if (gPool == NULL) {
		IOLog("WhyredGPU: no pool: buffers are allocated one by one\n");
	}
}

/* the first run of NPAGES free pages of the pool (gWhyredLock held), marked used; the page it starts at, or -1 */
static int64_t
whyredPoolTake(uint32_t npages)
{
	uint32_t run = 0;

	for (uint32_t i = 0; i < gPoolPages; i++) {
		run = gPoolUsed[i] ? 0 : run + 1;
		if (run == npages) {
			memset(gPoolUsed + (i + 1 - npages), 1, npages);
			return (int64_t)(i + 1 - npages);
		}
	}
	return -1;
}

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
	(void)PE_parse_boot_argn("whyredgpu_verbose", &gWhyredVerbose, sizeof(gWhyredVerbose));
	if (!gPoolTried) {
		gPoolTried = 1;
		whyredPoolInit();
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
		uint32_t metal = 0;

		IOLog("WhyredGPU: service started\n");
		if (PE_parse_boot_argn("whyredmetal", &metal, sizeof(metal)) && metal != 0) {
			(void)whyredMetalSet(1);
		}
	}
	gpu->release();
}

/*
 * Metal finds its device through the IOKit service of class IOAcceleratorES (the empty one the framebuffer driver publishes):
 * it reads MetalPluginName and MetalPluginClassName from it and loads /System/Library/Extensions/<name>.bundle. The properties
 * are put there only when a client asks (and taken away again), so the rest of the system keeps drawing as it did.
 */
static IOReturn
whyredMetalSet(uint32_t on)
{
	OSDictionary * match = IOService::serviceMatching("IOAcceleratorES");
	IOService * accel = match != NULL ? IOService::copyMatchingService(match) : NULL;

	if (match != NULL) {
		match->release();
	}
	if (accel == NULL) {
		IOLog("WhyredGPU: Metal: there is no IOAcceleratorES service\n");
		return kIOReturnNotFound;
	}
	if (on) {
		accel->setProperty("MetalPluginName", "WhyredMetal");
		accel->setProperty("MetalPluginClassName", "WhyredMetalDevice");
	} else {
		accel->removeProperty("MetalPluginName");
		accel->removeProperty("MetalPluginClassName");
	}
	IOLog("WhyredGPU: Metal: plug-in properties %s on IOAcceleratorES\n", on ? "set" : "removed");
	accel->release();
	return kIOReturnSuccess;
}

class WhyredGPUUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(WhyredGPUUserClient);

	task_t                          fTask;
	IOMemoryDescriptor *            fMemory[WHYRED_BUFFERS];   /* handle n is fMemory[n - 1] */
	uint64_t                        fAddress[WHYRED_BUFFERS];
	uint32_t                        fPoolAt[WHYRED_BUFFERS];   /* the pool page the buffer starts at ... */
	uint32_t                        fPoolN[WHYRED_BUFFERS];    /* ... and its length in pages (0: not from the pool) */

	IOReturn bufferNew(struct whyred_buf_new * request);
	IOReturn bufferFree(uint32_t handle);
	IOReturn submit(IOExternalMethodArguments * args, struct whyred_submit_out * out);
	IOReturn submitBuf(const struct whyred_submit_buf * request, struct whyred_submit_out * out);
	IOReturn run(uint32_t * words, size_t size, struct whyred_submit_out * out);

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
			if (fPoolN[i] != 0) {
				bzero(gPoolUsed + fPoolAt[i], fPoolN[i]);
				fPoolN[i] = 0;
			}
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
		*options = fPoolN[type - 1] != 0 ? kIOMapWriteCombineCache : 0;
		if (gWhyredVerbose) {
			IOLog("WhyredGPU: map of buffer %u (%s)\n", (unsigned)type, fPoolN[type - 1] != 0 ? "pool" : "own");
		}
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gWhyredLock);
	return ret;
}

IOReturn
WhyredGPUUserClient::bufferNew(struct whyred_buf_new * request)
{
	IOMemoryDescriptor * memory = NULL;
	uint64_t size = round_page_64(request->size);
	uint32_t slot = WHYRED_BUFFERS;
	uint32_t at = 0, npages = 0;

	if (request->size == 0 || size > WHYRED_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	if (gPool != NULL) {
		int64_t first;

		npages = (uint32_t)(size / WHYRED_POOL_PAGE);
		IOLockLock(gWhyredLock);
		first = whyredPoolTake(npages);
		IOLockUnlock(gWhyredLock);
		if (first >= 0) {
			at = (uint32_t)first;
			/* a piece of the pool's own (wired) descriptor: a physical-range descriptor made here panics when mapped or DMA'd */
			memory = IOSubMemoryDescriptor::withSubRange(gPool, (uint64_t)at * WHYRED_POOL_PAGE, size, kIODirectionInOut);
			if (memory == NULL) {
				IOLockLock(gWhyredLock);
				bzero(gPoolUsed + at, npages);
				IOLockUnlock(gWhyredLock);
			} else {
				/* what a service buffer is: zeroed (the pool's pieces were somebody's before) */
				bzero((uint8_t *)gPool->getBytesNoCopy() + (uint64_t)at * WHYRED_POOL_PAGE, size);
			}
		}
	}
	if (memory == NULL) {
		npages = 0;
		/* below 4 GB and in one piece: the GPU's page table is the identity of the first four gigabytes */
		memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		    kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapWriteCombineCache,
		    size, 0x00000000FFFFF000ULL);
	}
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
		if (npages != 0) {
			bzero(gPoolUsed + at, npages);
		}
		IOLockUnlock(gWhyredLock);
		memory->release();
		return kIOReturnNoResources;
	}
	fMemory[slot] = memory;
	fPoolAt[slot] = at;
	fPoolN[slot] = npages;
	fAddress[slot] = npages != 0 ? gPoolPhys + (uint64_t)at * WHYRED_POOL_PAGE : memory->getPhysicalSegment(0, NULL, kIOMemoryMapperNone);
	request->handle = slot + 1;
	request->address = fAddress[slot];
	request->size = size;
	IOLockUnlock(gWhyredLock);
	if (gWhyredVerbose) {
		IOLog("WhyredGPU: buffer %u: pa %llx size %llu%s\n", request->handle, request->address, size, npages != 0 ? " (pool)" : "");
	}
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
		if (fPoolN[handle - 1] != 0) {
			bzero(gPoolUsed + fPoolAt[handle - 1], fPoolN[handle - 1]);
			fPoolN[handle - 1] = 0;
		}
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
	return run(words, size, out);
}

IOReturn
WhyredGPUUserClient::run(uint32_t * words, size_t size, struct whyred_submit_out * out)
{
	IOReturn ret = kIOReturnSuccess;
	int status = 0;

	IOLockLock(gWhyredLock);
	out->fence = whyred_gpu_service_submit(words, (uint32_t)(size / 4), &status);
	if (out->fence != 0) {
		gWhyredSubmits++;
		gWhyredLast = out->fence;
	}
	IOLockUnlock(gWhyredLock);
	if (status != 0 || gWhyredVerbose || gWhyredSubmits <= 5) {
		IOLog("WhyredGPU: submit of %llu bytes: fence %u status %d, first words %08x %08x %08x %08x\n", (unsigned long long)size, out->fence, status, size >= 4 ? words[0] : 0, size >= 8 ? words[1] : 0, size >= 12 ? words[2] : 0, size >= 16 ? words[3] : 0);
	}
	out->status = (uint32_t)status;
	if (status != 0) {
		ret = kIOReturnIOError;
	}
	IOFree(words, size);
	return ret;
}

/* The stream is already in one of the client's buffers: copy it out of there, which avoids IOKit's large-input path. */
IOReturn
WhyredGPUUserClient::submitBuf(const struct whyred_submit_buf * request, struct whyred_submit_out * out)
{
	uint32_t * words;
	size_t size = (size_t)request->dwords * 4;
	IOReturn ret = kIOReturnBadArgument;

	if (request->dwords == 0 || size > WHYRED_SUBMIT_MAX || (request->offset & 3) != 0) {
		return kIOReturnBadArgument;
	}
	words = (uint32_t *)IOMalloc(size);
	if (words == NULL) {
		return kIOReturnNoMemory;
	}
	IOLockLock(gWhyredLock);
	if (request->handle >= 1 && request->handle <= WHYRED_BUFFERS && fMemory[request->handle - 1] != NULL &&
	    (uint64_t)request->offset + size <= fMemory[request->handle - 1]->getLength()) {
		if (fMemory[request->handle - 1]->readBytes(request->offset, words, size) == size) {
			ret = kIOReturnSuccess;
		} else {
			ret = kIOReturnVMError;
		}
	}
	IOLockUnlock(gWhyredLock);
	if (ret != kIOReturnSuccess) {
		IOFree(words, size);
		return ret;
	}
	return run(words, size, out);
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
	case kWhyredSubmitBuf:
		if (args->structureInput == NULL || size < sizeof(struct whyred_submit_buf)) {
			return kIOReturnBadArgument;
		}
		if (!whyred_gpu_service_ready()) {
			return kIOReturnNotReady;
		}
		ret = submitBuf((const struct whyred_submit_buf *)args->structureInput, &r.out);
		outSize = sizeof(r.out);
		break;
	case kWhyredMetal:
		if (args->structureInput == NULL || size < sizeof(uint32_t)) {
			return kIOReturnBadArgument;
		}
		ret = whyredMetalSet(*(const uint32_t *)args->structureInput);
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
	words = fPoolN[bn.handle - 1] != 0 ? (volatile uint32_t *)((uint8_t *)gPool->getBytesNoCopy() + (uint64_t)fPoolAt[bn.handle - 1] * WHYRED_POOL_PAGE) :
	    (volatile uint32_t *)((IOBufferMemoryDescriptor *)fMemory[bn.handle - 1])->getBytesNoCopy();

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
