#!/usr/bin/env python3
"""Build the native OLMoE Edge/Segment worker and gates, never the upstream tree.

This is an inference and residency gate. It does not package or claim a
complete native Windows Lumabri service. No checkpoint is downloaded here.
"""
import argparse
from pathlib import Path
import subprocess
import sys


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine')
    parser.add_argument('--only',choices=('all','inference','contracts'),default='all')
    parser.add_argument('--output',default='build/windows-segment')
    parser.add_argument('--cc',default='gcc')
    parser.add_argument('--ar',default='ar')
    parser.add_argument('--nm',default='nm')
    parser.add_argument('--windres',help='defaults to the windres matching --cc')
    args=parser.parse_args()
    if args.only != 'contracts' and not args.engine:
        parser.error('--engine is required for inference builds')
    root=Path(__file__).resolve().parents[1]
    output=Path(args.output).resolve()
    output.mkdir(parents=True,exist_ok=True)
    source=output/'colibri-source'
    def run(command):
        subprocess.run([str(v) for v in command],cwd=root,check=True)
    flags=['-I'+str(root),'-I'+str(source),'-O2','-Wall','-Wextra',
           '-D_FILE_OFFSET_BITS=64','-D_WIN32_WINNT=0x0A00']
    resource=output/'windows_manifest.o'
    windres=args.windres or (args.cc[:-3]+'windres' if args.cc.endswith('gcc') else 'windres')
    run([windres,'-I',root,root/'src/platform/lumabri_windows.rc','-O','coff','-o',resource])
    run([args.cc,*flags,'-Werror','-municode',root/'tests/c/test_owned_process_windows.c',
         '-static','-o',output/'test_owned_process_windows.exe'])
    run([args.cc,*flags,'-Werror','-Wno-unused-function','-Wno-unused-variable','-municode',
         root/'lumabri.c',root/'lumabri_machine.c',resource,'-static','-lwinpthread','-lws2_32',
         '-lbcrypt','-liphlpapi','-lpsapi','-ladvapi32','-ldxgi','-ldxguid',
         '-o',output/'lumabri.exe'])
    if args.only != 'contracts':
        run([sys.executable,'-X','utf8','tools/prepare_engine_source.py','--source',
             Path(args.engine).resolve(),'--output',source])
        run([sys.executable,'-X','utf8','tools/prepare_resident_adapters.py','--copy',source])
        memory=root/'src/runtime/lumabri_engine_memory_override.h'
        inputs=root/'src/runtime/lumabri_engine_io_windows_override.h'
        objects=[]
        for name in ('olmoe','segment_runtime','edge_runtime','engine_memory','engine_io_windows'):
            file=(source/(name+'.c') if name in ('olmoe','segment_runtime','edge_runtime')
                  else root/'src/runtime'/('lumabri_'+name+'.c'))
            extra=['-Werror']
            if name=='olmoe':
                # Upstream embedding units contain unused standalone helpers.
                extra=['-Werror=implicit-function-declaration','-Wno-unused-function',
                       '-DCOLI_SEGMENT_ADAPTER','-DCOLI_EDGE_ADAPTER',
                       '-DOLMOE_NO_MAIN','-DLUMABRI_P2P','-include','pthread.h',
                       '-include',root/'lumi_v4_ext.h']
            if name not in ('engine_memory','engine_io_windows'):
                extra+=['-include',memory,'-include',inputs]
            obj=output/(name+'.o')
            run([args.cc,*flags,*extra,'-c',file,'-o',obj]);objects.append(obj)
        archive=output/'liblumabri_windows_olmoe.a'
        run([args.ar,'rcs',archive,*objects])
        run([sys.executable,'tools/check_engine_memory.py',archive,'--nm',args.nm,'--windows-input'])
        run([args.cc,*flags,'-Werror',root/'tests/c/test_segment_windows.c',resource,archive,
             '-static','-lwinpthread','-lpsapi','-lm','-o',output/'test_segment_windows.exe'])
        print('Native inference gate:',output/'test_segment_windows.exe')
        libraries=['-static','-lwinpthread','-lws2_32','-lbcrypt','-liphlpapi',
                   '-lpsapi','-ladvapi32','-ldxgi','-ldxguid','-lm']
        protocol=[root/'lumabri_segment.c',root/'lumabri_segment_discovery.c']
        native_input=output/'checkpoint_windows.o'
        run([args.cc,*flags,'-Werror','-Wno-unused-function',
             '-c',root/'src/runtime/lumabri_checkpoint_windows.c','-o',native_input])
        run([args.cc,*flags,'-Werror','-DLMB_SEGMENT_OLMOE_ONLY','-include',memory,'-municode',
             root/'segment_node.c',*protocol,root/'lumabri_machine.c',root/'lumabri_run_gate.c',
             native_input,resource,archive,*libraries,'-o',output/'segment_node.exe'])
        run([args.cc,*flags,'-Werror','-DLMB_SEGMENT_OLMOE_ONLY','-include',memory,'-municode',
             root/'segment_chat.c',*protocol,root/'lumabri_machine.c',root/'lumabri_sampling.c',
             root/'src/runtime/lumabri_vision.c',root/'src/runtime/lumabri_image.c',
             native_input,resource,archive,*libraries,'-o',output/'segment_chat.exe'])
        run([args.cc,*flags,'-Werror','-Wno-unused-function','-municode',
             root/'tests/c/test_segment_worker_windows.c',*protocol,resource,archive,*libraries,
             '-o',output/'test_segment_worker_windows.exe'])
        run([args.cc,*flags,'-Werror','-Wno-unused-function',
             root/'tests/c/test_checkpoint_windows.c',native_input,resource,archive,*libraries,
             '-o',output/'test_checkpoint_windows.exe'])
        run([args.cc,*flags,'-Werror','-Wno-unused-function','-municode',
             root/'tests/c/test_donor_windows.c',*protocol,resource,archive,*libraries,
             '-o',output/'test_donor_windows.exe'])
    if args.only == 'inference':
        return
    run([args.cc,*flags,'-Werror','-Wno-unused-function','-municode',
         root/'tests/c/test_allocation_windows.c',resource,'-static','-lwinpthread','-lws2_32',
         '-lbcrypt','-ladvapi32','-o',output/'test_allocation_windows.exe'])
    run([args.cc,*flags,'-Werror','-Wno-unused-function','-municode',
         root/'tests/c/test_manager_windows.c','-static','-lwinpthread','-lws2_32',
         '-lbcrypt','-ladvapi32','-o',output/'test_manager_windows.exe'])
    run([args.cc,*flags,'-Werror',root/'tests/c/test_machine_windows.c',
         root/'lumabri_machine.c','-static','-lwinpthread','-lws2_32','-liphlpapi',
         '-lpsapi','-ladvapi32','-ldxgi','-ldxguid','-o',output/'test_machine_windows.exe'])
    run([args.cc,*flags,'-Werror',root/'tests/c/test_planner_windows.c',
         '-static','-o',output/'test_planner_windows.exe'])
    run([args.cc,*flags,'-Werror','-municode',root/'tests/c/test_tui_windows.c',
         root/'src/ui/lumabri_tui.c','-static','-lwinpthread',
         '-o',output/'test_tui_windows.exe'])
    run([args.cc,*flags,'-Werror',root/'tests/c/test_segment_v2.c',
         root/'lumabri_segment.c','-static','-lwinpthread',
         '-o',output/'test_segment_v2.exe'])
    run([args.cc,*flags,'-Werror',root/'tests/c/test_run_gate.c',root/'lumabri_run_gate.c',
         '-static','-lwinpthread','-o',output/'test_run_gate.exe'])
    run([args.cc,*flags,'-Werror','-Wno-unused-function',
         root/'tests/c/test_socket_transport.c','-static','-lwinpthread','-lws2_32',
         '-o',output/'test_socket_transport.exe'])
    run([args.cc,*flags,'-Werror','-Wno-unused-function',
         root/'tests/c/test_secure_transport.c','-static','-lwinpthread','-lws2_32','-lbcrypt',
         '-o',output/'test_secure_transport.exe'])
    run([args.cc,*flags,'-Werror','-Wno-unused-function',
         root/'tests/c/test_crypto_vectors.c','-static','-o',output/'test_crypto_vectors.exe'])
    run([args.cc,*flags,'-Werror','-Wno-unused-function','-municode',
         root/'tests/c/test_peer_identity_windows.c','-static','-lwinpthread',
         '-lws2_32','-lbcrypt','-ladvapi32','-o',output/'test_peer_identity_windows.exe'])
    run([args.cc,*flags,'-Werror',root/'tests/c/test_runtime_probe.c',
         '-static','-o',output/'test_runtime_probe.exe'])
    run([args.cc,*flags,'-Werror','-municode',root/'tests/c/test_process_probe_windows.c',
         '-static','-o',output/'test_process_probe_windows.exe'])
    run([args.cc,*flags,'-Werror','-Wno-unused-function','-municode',
         root/'tests/c/test_service_windows.c','-static','-lwinpthread',
         '-lws2_32','-lbcrypt','-ladvapi32','-o',output/'test_service_windows.exe'])
    run([args.cc,*flags,'-Werror','-Wno-unused-function','-municode',
         root/'tests/c/test_compute_windows.c','-static','-lwinpthread',
         '-lws2_32','-lbcrypt','-ladvapi32','-o',output/'test_compute_windows.exe'])
    run([args.cc,*flags,'-Werror','-Wno-unused-function','-municode',
         root/'tests/c/test_node_control_windows.c','-static','-lwinpthread',
         '-lws2_32','-lbcrypt','-ladvapi32','-o',output/'test_node_control_windows.exe'])
    run([args.cc,*flags,'-Werror','-Wno-unused-function','-municode',
         root/'tests/c/test_home_network_windows.c','-static','-lwinpthread',
         '-lws2_32','-lbcrypt','-ladvapi32','-liphlpapi','-o',output/'test_home_network_windows.exe'])
    print('Native contract gates:',output)


if __name__=='__main__':
    main()
