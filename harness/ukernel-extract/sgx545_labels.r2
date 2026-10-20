f stub.insn0 @ 0x0
CCu assert trap: 0xAD002600 -> HostCtl.ui32AssertFail ([sa1,+#14]), then ba 0xb9f8; nothing targets it @ 0x0
f edm.lock_dpm_clear @ 0xb9f8
f dpm.state_clear @ 0xba08
CCu DPM_TASK_STATE=CLEAR, wait EVENT_STATUS.DPM_STATE_CLEAR, ack EVENT_HOST_CLEAR @ 0xba08
f hwperf.log @ 0xcbc8
CCu HWPerf logger: entry = [sa0+43] (HWPerf CB) + 12 + woff*100 @ 0xcbc8
f edm.task.boot @ 0xd60
CCu boot task, entered via PDS #10 (EVENT_OTHER_PDS_EXEC from the init script) @ 0xd60
f edm.task.pds11 @ 0xdf0
CCu entered via PDS #11 (DMAs TA3DCtl into the sa registers), emitpds'd by the boot task @ 0xdf0
f edm.task.init_done @ 0xe20
CCu PDS #12 task: installs PDS #13, programs EVENT_TIMER, HostCtl.ui32InitStatus = INIT_COMPLETE @ 0xe20
f edm.event.general @ 0x30
CCu PDS #13 default target: every event outside the fast paths @ 0x30
f edm.event.dpm_store @ 0x7050
CCu PDS #13: only DPM_INITEND|DPM_CONTROL_STORE|DPM_STATE_STORE pending @ 0x7050
f edm.event.dpm_clear @ 0x2cd0
CCu PDS #13: only DPM_CONTROL_CLEAR pending @ 0x2cd0
f edm.event.csw_flush @ 0x5670
CCu PDS #13: only ISP2_ZLS_CSW_FINISHED|OTPM_INV|TPC_FLUSH pending @ 0x5670
f edm.work.dpm_store @ 0x73a0
f edm.work.dpm_clear @ 0x2ea0
f edm.work.csw_flush @ 0x5740
CCu PDS #14 task: requests 0x4000f; clears them from TA3DCtl dw31, then the dpm_store paths @ 0x73a0
CCu PDS #14 task: requests 0x120080 @ 0x2ea0
CCu PDS #14 task: all other requests @ 0x5740
f edm.request @ 0xbab0
CCu post deferred work: TA3DCtl dw31 |= r16; if new bits, emitpds PDS #14 with ir0 = r16 @ 0xbab0
CCu patched: emitpds control word for PDS #11 (data size 2) @ 0xd60
CCu patched: PDS #11 address >> 4 @ 0xd70
CCu patched: TA3DCtl device address @ 0xd78
CCu patched: emitpds control word for PDS #12 (data size 1) @ 0xdb0
CCu patched: PDS #12 address >> 4 @ 0xdc0
CCu patched: PDS #13 address -> EVENT_OTHER_PDS_EXEC @ 0xe40
CCu patched: PDS #13 data size (6) -> EVENT_OTHER_PDS_DATA @ 0xe50
CCu patched: emitpds control word for PDS #14 (data size 3) @ 0xbaf8
CCu patched: PDS #14 address >> 4 @ 0xbb10
f edm.ccb.fetch @ 0x4c0
CCu kick event (pa1 bit 8): cmd = [sa0+22] (CCB) + ReadOffset*32 -> r4 service, r5 cache ctl, r6-r11 ui32Data[0..5] @ 0x4c0
CCu cache control r5: bit1 INVAL_BIF_PD / bit0 INVAL_BIF_PT -> 0x158a, bit3 INVAL_DATA -> CACHE_CTRL @ 0x518
CCu ReadOffset = (ReadOffset+1)&0xff, then jump to the handler in r4 @ 0x5f8
CCu CLEANUP: r6 = type (RT 1..PB 5) -> sa10 pending bit, r7 = HW data addr -> TA3DCtl dw32 @ 0x9d0
CCu SETHWPERFSTATUS: r6 bit0 RESET_COUNTERS resets the HWPerf CB; the rest -> sa19 (HWPerf status) @ 0xab8
