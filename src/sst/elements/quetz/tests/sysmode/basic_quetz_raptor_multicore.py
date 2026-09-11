"""Two real CPUs, independent trace/mailbox slots, and one shared SST window.

Native RAM remains QEMU-owned. Per-core trace memories do not drive guest data.
Only the synchronous shared window supplies the cross-core SST data exchange.
"""
import os
import sst

primary = os.environ['QUETZ_EXE']
secondary = os.environ['QUETZ_SECONDARY_EXE']
for path in (primary, secondary):
    if not os.path.isfile(path) or any(char in path for char in ', \t\n'):
        raise ValueError('multicore diagnostic requires existing ELF paths without QEMU delimiters')
os.environ['QUETZ_SST_WIN_START'] = '0x71000000'
os.environ['QUETZ_SST_WIN_END'] = '0x7100003f'
for name in ('QUETZ_MMIO_PAYLOAD', 'QUETZ_MMIO_START', 'QUETZ_MMIO_END',
             'QUETZ_IRQ_LINES', 'QUETZ_BSP_PROFILE', 'QUETZ_BSP_DISCOVER'):
    os.environ.pop(name, None)
cpu = sst.Component('cpu', 'quetz.QuetzComponent')
cpu.addParams({'verbose': 1, 'clock': '1GHz', 'vcpu_count': 2,
               'maxcorequeue': 64, 'maxtranscore': 16, 'maxissuepercycle': 8,
               'cachelinesize': 64, 'qemu': os.environ['QUETZ_QEMU'],
               'qemu_plugin': os.environ.get('QUETZ_PLUGIN', '/opt/sst/libexec/libqemu_sst_plugin.so'),
               'executable': primary, 'system_mode': 1, 'system_mode_loader': '-kernel',
               'qemu_args': f'-machine raptor-core2,secondary-kernel={secondary} '
                            '-smp 2 -accel tcg,thread=single -display none -monitor none '
                            '-serial null -serial stdio -serial null',
               'window_big_endian': 1, 'sst_window_cache': 0,
               'appstdout': os.environ['QUETZ_STDOUT_FILE'],
               'filter_unmatched_regions': 1})
for index, (kind, params) in enumerate((
    ('quetz.TestFinisherRegionHandler', {'start': 0x0700fff0, 'end': 0x0700fff7}),
    ('quetz.UartRegionHandler', {'start': 0xfc064000, 'end': 0xfc06401f, 'tx_offset': 12}),
    ('quetz.ForwardRegionHandler', {'start': 0x07000000, 'end': 0x0700ffff}),
)):
    cpu.setSubComponent('region_handler', kind, index).addParams(params)
cpu.enableAllStatistics()
router = sst.Component('router', 'merlin.hr_router')
router.addParams({'xbar_bw': '128GB/s', 'id': '0', 'input_buf_size': '1KB',
                  'num_ports': '3', 'flit_size': '72B', 'output_buf_size': '1KB',
                  'link_bw': '128GB/s', 'topology': 'merlin.singlerouter'})
router.setSubComponent('topology', 'merlin.singlerouter')
for core in range(2):
    trace = sst.Component(f'trace{core}', 'memHierarchy.MemController')
    trace.addParams({'clock': '1GHz', 'addr_range_start': 0x07000000,
                     'addr_range_end': 0x0700ffff})
    trace.setSubComponent('backend', 'memHierarchy.simpleMem').addParams(
        {'access_time': '100ns', 'mem_size': '65536B'})
    sst.Link(f'trace_link{core}').connect((cpu, f'cache_link_{core}', '1ns'),
                                        (trace, 'highlink', '1ns'))
    interface = cpu.setSubComponent('mmio', 'memHierarchy.standardInterface', core)
    nic = interface.setSubComponent('lowlink', 'memHierarchy.MemNIC')
    nic.addParams({'group': 0, 'destinations': [2], 'network_bw': '25GB/s'})
    sst.Link(f'mailbox{core}').connect((nic, 'port', '1ns'), (router, f'port{core}', '1ns'))
memory = sst.Component('shared_memory', 'memHierarchy.MemController')
memory.addParams({'clock': '1GHz', 'addr_range_start': 0x71000000, 'addr_range_end': 0x7100003f})
memory.setSubComponent('backend', 'memHierarchy.simpleMem').addParams(
    {'access_time': '100ns', 'mem_size': '64B'})
memlink = memory.setSubComponent('highlink', 'memHierarchy.MemLink')
directory = sst.Component('directory', 'memHierarchy.DirectoryController')
directory.addParams({'clock': '1GHz', 'coherence_protocol': 'MESI', 'cache_line_size': 64,
                     'entry_cache_size': 4096, 'mshr_num_entries': 256,
                     'addr_range_start': 0x71000000, 'addr_range_end': 0x7100003f})
network = directory.setSubComponent('highlink', 'memHierarchy.MemNIC')
network.addParams({'group': 2, 'sources': [0], 'network_bw': '25GB/s'})
sst.Link('directory_network').connect((network, 'port', '1ns'), (router, 'port2', '1ns'))
sst.Link('directory_memory').connect((memlink, 'port', '1ns'), (directory, 'lowlink', '1ns'))
sst.setProgramOption('timebase', '1ps')
sst.setStatisticLoadLevel(4)
sst.setStatisticOutput('sst.statOutputCSV', {'filepath': os.environ['QUETZ_STATS_OUT']})
