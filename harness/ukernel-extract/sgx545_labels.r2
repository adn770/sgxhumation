f stub.insn0 @ 0x0
CCu signature 0xAD002600 -> [sa1,+#14] (HostCtl dw14), then ba 0xb9f8; no branch or PDS entry targets it @ 0x0
f edm.lock_dpm_clear @ 0xb9f8
f dpm.state_clear @ 0xba08
CCu DPM_TASK_STATE=CLEAR, wait EVENT_STATUS.DPM_STATE_CLEAR, ack EVENT_HOST_CLEAR @ 0xba08
f hwperf.log @ 0xcbc8
CCu HWPerf logger: entry = [sa0+43] (HWPerf CB) + 12 + woff*100 @ 0xcbc8
f edm.task.boot @ 0xd60
CCu boot task, entered via PDS #10 (EVENT_OTHER_PDS_EXEC from the init script) @ 0xd60
f edm.task.pds11 @ 0xdf0
CCu entered via PDS #11 (DMAs TA3DCtl into the sa registers), emitpds'd by the boot task @ 0xdf0
f edm.task.pds12 @ 0xe20
CCu entered via PDS #12, emitpds'd by the boot task @ 0xe20
f edm.event.general @ 0x30
CCu PDS #13 default target: every event outside the fast paths @ 0x30
f edm.event.dpm_store @ 0x7050
CCu PDS #13: only DPM_INITEND|DPM_CONTROL_STORE|DPM_STATE_STORE pending @ 0x7050
f edm.event.dpm_clear @ 0x2cd0
CCu PDS #13: only DPM_CONTROL_CLEAR pending @ 0x2cd0
f edm.event.csw_flush @ 0x5670
CCu PDS #13: only ISP2_ZLS_CSW_FINISHED|OTPM_INV|TPC_FLUSH pending @ 0x5670
f edm.task.pds14a @ 0x73a0
f edm.task.pds14b @ 0x2ea0
f edm.task.pds14c @ 0x5740
CCu patched: emitpds control word for PDS #11 (data size 2) @ 0xd60
CCu patched: PDS #11 address >> 4 @ 0xd70
CCu patched: TA3DCtl device address @ 0xd78
CCu patched: emitpds control word for PDS #12 (data size 1) @ 0xdb0
CCu patched: PDS #12 address >> 4 @ 0xdc0
CCu patched: PDS #13 address -> EVENT_OTHER_PDS_EXEC @ 0xe40
CCu patched: PDS #13 data size (6) -> EVENT_OTHER_PDS_DATA @ 0xe50
CCu patched: emitpds control word for PDS #14 (data size 3) @ 0xbaf8
CCu patched: PDS #14 address >> 4 @ 0xbb10
