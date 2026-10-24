#!/usr/bin/env python3
"""Build the minimal Apple Device Tree that the WHYRED/SWEET xnu needs to boot.

Only what the early boot path actually reads (see NOTES.md); every MMIO
address comes from the live FDT.
Usage: mkadt.py OUT.adt [--dump] [--qemu] [--highmem] [--device whyred|sweet]
"""
import hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

# Kernel-managed RAM: above every Qualcomm XPU carve-out (see NOTES.md)
PHYS_BASE = 0xb0000000
MEM_SIZE = 0x4d000000
PANIC_LOG_SIZE = 0x100000
HIGH_PHYS_BASE = 0x100000000
HIGH_MEM_SIZE = 0xfcc00000
SOC_BASE = 0x01000000

# whyred hardware (SDM636 / SDM660)
WHYRED_GICD, WHYRED_GICD_SIZE = 0x17a00000, 0x10000
WHYRED_GICR, WHYRED_GICR_SIZE = 0x17b00000, 0x100000
WHYRED_FB, WHYRED_FB_SIZE = 0x9d400000, 0x2400000
WHYRED_ECID = 0x5748595245440001  # 'WHYRED' + 1
WHYRED_FB_W, WHYRED_FB_H = 1080, 2160
WHYRED_SDHCI, WHYRED_SDHCI_SIZE = 0x0c0c4000, 0x1000

# sweet hardware (SM7150)
SWEET_GICD, SWEET_GICD_SIZE = 0x17a00000, 0x10000
SWEET_GICR, SWEET_GICR_SIZE = 0x17a60000, 0x100000
SWEET_FB, SWEET_FB_SIZE = 0x9c000000, 0x1700000
SWEET_ECID = 0x5357454554000001  # 'SWEET' + 1
SWEET_FB_W, SWEET_FB_H = 1080, 2400
SWEET_UFS, SWEET_UFS_SIZE = 0x01d84000, 0x3000

TIMEBASE = 19200000
VTIMER_INTID = 19

# QEMU virt
QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE = 0x08000000, 0x080a0000, 62500000
QEMU_VTIMER_INTID = 27

BOOT_MANIFEST_HASH = hashlib.sha384(b'whyred boot manifest').digest()

D22AP_ADT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../research/ipsw-x/d22ap.adt')


def d22ap_product(path=D22AP_ADT):
    """/product properties of the real D22AP tree."""
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def build(seed=None, qemu=False, highmem=False, device='whyred'):
    """Build Apple Device Tree for target device (whyred or sweet)."""
    phys_base, mem_size = (HIGH_PHYS_BASE, HIGH_MEM_SIZE) if highmem else (PHYS_BASE, MEM_SIZE)
    panic_base = phys_base + mem_size - PANIC_LOG_SIZE
    mem_size -= PANIC_LOG_SIZE

    is_whyred = (device == 'whyred')
    if qemu:
        gicd, gicr, timebase = QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE
        vtimer_intid = QEMU_VTIMER_INTID
    elif is_whyred:
        gicd, gicr, timebase = WHYRED_GICD, WHYRED_GICR, TIMEBASE
        vtimer_intid = VTIMER_INTID
    else:
        gicd, gicr, timebase = SWEET_GICD, SWEET_GICR, TIMEBASE
        vtimer_intid = VTIMER_INTID

    fb_base = WHYRED_FB if is_whyred else SWEET_FB
    fb_size = WHYRED_FB_SIZE if is_whyred else SWEET_FB_SIZE
    fb_w = WHYRED_FB_W if is_whyred else SWEET_FB_W
    fb_h = WHYRED_FB_H if is_whyred else SWEET_FB_H
    fb_stride = fb_w * 4
    ecid = WHYRED_ECID if is_whyred else SWEET_ECID
    dev_name = 'whyred' if is_whyred else 'sweet'
    soc_compat = 'qcom,sdm636' if is_whyred else 'qcom,sm7150'

    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('embedded-panic-log-size', u32(PANIC_LOG_SIZE)),
        ('firmware-version', cstr(f'{dev_name}-shim')),
        ('system-firmware-version', cstr(f'{dev_name}-shim')),
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        ('crypto-hash-method', cstr('sha2-384')),
        ('display-scale', u32(3)),
        ('unique-chip-id', struct.pack('<Q', ecid)),
        ('chip-id', u32(0x8015)),
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # CPUs topology
    cpu_nodes = []
    if qemu:
        cpu_nodes.append(node('cpu0', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr('ARM,v8')),
            ('reg', u32(0)),
            ('cpu-id', u32(0)),
            ('cluster-id', u32(0)),
            ('cluster-type', u32(0)),
            ('state', cstr('running')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(2000000000)),
        ]))
    elif is_whyred:
        # whyred Kryo 260: 4x Cortex-A53 Silver + 4x Cortex-A73 Gold
        sdm636_mpidrs = [0x000, 0x001, 0x002, 0x003, 0x100, 0x101, 0x102, 0x103]
        for core in range(8):
            is_gold = (core >= 4)
            cpu_nodes.append(node(f'cpu{core}', [
                ph(),
                ('device_type', cstr('cpu')),
                ('compatible', cstr('qcom,kryo260') + cstr('ARM,v8')),
                ('reg', u32(sdm636_mpidrs[core])),
                ('cpu-id', u32(core)),
                ('cluster-id', u32(1 if is_gold else 0)),
                ('cluster-type', u32(0)),
                ('die-cluster-id', u32(0)),
                ('cluster-core-id', u32(core & 3)),
                ('l2-cache-id', u32(1 if is_gold else 0)),
                ('l3-cache-id', u32(0)),
                ('state', cstr('running' if core == 0 else 'stopped')),
                ('timebase-frequency', u32(timebase)),
                ('clock-frequency', u32(1804800000 if is_gold else 1612800000)),
                ('bus-frequency', u32(100000000)),
                ('memory-frequency', u32(1333000000)),
                ('peripheral-frequency', u32(timebase)),
                ('fixed-frequency', u32(timebase)),
            ]))
    else:
        # sweet Kryo 470
        for core in range(8):
            cpu_nodes.append(node(f'cpu{core}', [
                ph(),
                ('device_type', cstr('cpu')),
                ('compatible', cstr('qcom,kryo470') + cstr('ARM,v8')),
                ('reg', u32(core << 8)),
                ('cpu-id', u32(core)),
                ('cluster-id', u32(0)),
                ('cluster-type', u32(0)),
                ('die-cluster-id', u32(0)),
                ('cluster-core-id', u32(core)),
                ('l2-cache-id', u32(core)),
                ('l3-cache-id', u32(0)),
                ('state', cstr('running' if core == 0 else 'stopped')),
                ('timebase-frequency', u32(timebase)),
                ('clock-frequency', u32(2304000000 if core >= 6 else 1804800000)),
                ('bus-frequency', u32(100000000)),
                ('memory-frequency', u32(1866000000)),
                ('peripheral-frequency', u32(timebase)),
                ('fixed-frequency', u32(timebase)),
            ]))

    cpus = node('cpus', [ph(), ('#address-cells', u32(1)),
                         ('#size-cells', u32(0))], cpu_nodes)

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(vtimer_intid)),
        ('reg', u64(gicd - SOC_BASE, 0x10000, gicr - SOC_BASE, 0x100000)),
    ])

    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr(f'{dev_name}-io')),
        ('compatible', cstr(f'{dev_name}-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic,
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr(f'{dev_name},display')),
            ('canvas-width', u32(1125)),
            ('canvas-height', u32(2250 if is_whyred else 2436)),
        ])])

    fbcon = node(f'{dev_name}-fbcon', [
        ph(),
        ('reg', u64(fb_base, fb_size)),
        ('width', u32(fb_w)),
        ('height', u32(fb_h)),
        ('stride', u32(fb_stride)),
    ])

    storage_nodes = []
    if not qemu:
        if is_whyred:
            storage_nodes.append(node('whyred-sdhci', [
                ph(),
                ('compatible', cstr('qcom,sdhci-msm-v5') + cstr('qcom,sdhci')),
                ('reg', u64(WHYRED_SDHCI, WHYRED_SDHCI_SIZE)),
            ]))
        else:
            storage_nodes.append(node('sweet-ufs', [
                ph(),
                ('compatible', cstr('qcom,ufshc')),
                ('reg', u64(SWEET_UFS, SWEET_UFS_SIZE)),
            ]))

    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])

    product_props = d22ap_product()
    product = [node('product', [ph()] + product_props)] if product_props else []

    pram = node('pram', [ph(), ('reg', u64(panic_base) + u64(PANIC_LOG_SIZE))])

    return node('device-tree', [
        ph(),
        ('compatible', cstr('D22AP') + cstr('iPhone10,3') + cstr(f'xiaomi,{dev_name}') + cstr(soc_compat) + cstr('AppleARM')),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons, pram] + product + storage_nodes)


if __name__ == '__main__':
    dev = 'sweet' if '--sweet' in sys.argv else 'whyred'
    for arg in sys.argv:
        if arg.startswith('--device='):
            dev = arg.split('=', 1)[1]
    root = build(qemu='--qemu' in sys.argv, highmem='--highmem' in sys.argv, device=dev)
    blob = adt.serialize(root)
    out_file = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('--') else 'whyred.adt'
    open(out_file, 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{out_file}: {len(blob)} bytes (target: {dev})', file=sys.stderr)


if __name__ == '__main__':
    root = build(qemu='--qemu' in sys.argv, highmem='--highmem' in sys.argv)
    blob = adt.serialize(root)
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{sys.argv[1]}: {len(blob)} bytes', file=sys.stderr)
