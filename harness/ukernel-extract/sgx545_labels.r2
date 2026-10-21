f stub.insn0 @ 0x0
"CCu assert trap: 0xAD002600 -> HostCtl.ui32AssertFail ([sa1,+#14]), then ba 0xb9f8; nothing targets it" @ 0x0
f edm.lock_dpm_clear @ 0xb9f8
f dpm.state_clear @ 0xba08
"CCu DPM_TASK_STATE=CLEAR, wait EVENT_STATUS.DPM_STATE_CLEAR, ack EVENT_HOST_CLEAR" @ 0xba08
f hwperf.log @ 0xcbc8
"CCu HWPerf logger: entry = [sa0+43] (HWPerf CB) + 12 + woff*100" @ 0xcbc8
f edm.task.boot @ 0xd60
"CCu boot task, entered via PDS #10 (EVENT_OTHER_PDS_EXEC from the init script)" @ 0xd60
f edm.task.pds11 @ 0xdf0
"CCu entered via PDS #11 (DMAs TA3DCtl into the sa registers), emitpds'd by the boot task" @ 0xdf0
f edm.task.init_done @ 0xe20
"CCu PDS #12 task: installs PDS #13, programs EVENT_TIMER, HostCtl.ui32InitStatus = INIT_COMPLETE" @ 0xe20
f edm.event.general @ 0x30
"CCu PDS #13 default target: every event outside the fast paths" @ 0x30
f edm.event.dpm_store @ 0x7050
"CCu PDS #13: only DPM_INITEND|DPM_CONTROL_STORE|DPM_STATE_STORE pending" @ 0x7050
f edm.event.dpm_clear @ 0x2cd0
"CCu PDS #13: only DPM_CONTROL_CLEAR pending" @ 0x2cd0
f edm.event.csw_flush @ 0x5670
"CCu PDS #13: only ISP2_ZLS_CSW_FINISHED|OTPM_INV|TPC_FLUSH pending" @ 0x5670
f edm.work.dpm_store @ 0x73a0
f edm.work.dpm_clear @ 0x2ea0
f edm.work.csw_flush @ 0x5740
"CCu PDS #14 task: requests 0x4000f; clears them from TA3DCtl dw31, then the dpm_store paths" @ 0x73a0
"CCu PDS #14 task: requests 0x120080" @ 0x2ea0
"CCu PDS #14 task: all other requests" @ 0x5740
f edm.request @ 0xbab0
"CCu post deferred work: TA3DCtl dw31 |= r16; if new bits, emitpds PDS #14 with ir0 = r16" @ 0xbab0
"CCu patched: emitpds control word for PDS #11 (data size 2)" @ 0xd60
"CCu patched: PDS #11 address >> 4" @ 0xd70
"CCu patched: TA3DCtl device address" @ 0xd78
"CCu patched: emitpds control word for PDS #12 (data size 1)" @ 0xdb0
"CCu patched: PDS #12 address >> 4" @ 0xdc0
"CCu patched: PDS #13 address -> EVENT_OTHER_PDS_EXEC" @ 0xe40
"CCu patched: PDS #13 data size (6) -> EVENT_OTHER_PDS_DATA" @ 0xe50
"CCu patched: emitpds control word for PDS #14 (data size 3)" @ 0xbaf8
"CCu patched: PDS #14 address >> 4" @ 0xbb10
f edm.ccb.fetch @ 0x4c0
"CCu kick event (pa1 bit 8): cmd = [sa0+22] (CCB) + ReadOffset*32 -> r4 service, r5 cache ctl, r6-r11 ui32Data[0..5]" @ 0x4c0
"CCu cache control r5: bit1 INVAL_BIF_PD / bit0 INVAL_BIF_PT -> 0x158a, bit3 INVAL_DATA -> CACHE_CTRL" @ 0x518
"CCu ReadOffset = (ReadOffset+1)&0xff, then jump to the handler in r4" @ 0x5f8
"CCu CLEANUP: r6 = type (RT 1..PB 5) -> sa10 pending bit, r7 = HW data addr -> TA3DCtl dw32" @ 0x9d0
"CCu SETHWPERFSTATUS: r6 bit0 RESET_COUNTERS resets the HWPerf CB; the rest -> sa19 (HWPerf status)" @ 0xab8
f edm.schedule @ 0xb08
"CCu scheduler: sa11=0, sa12=3, post 0x40000 if 3D/transfer queued, 0x20000 if TA queued" @ 0xb08
"CCu TA_FINISHED (request 0x80): ack EVENT_HOST_CLEAR 0x2000, run edm.ta.finished" @ 0x2f48
"CCu request 0x20000 (TA work queued): edm.ta.pick" @ 0x2f80
f edm.ta.finished @ 0x3030
"CCu TA finished; sa8 bits 9/10 select the partial-render paths" @ 0x3030
f edm.ta.finished.normal @ 0x3218
"CCu clear READY, TATQ read-op, PB AllocPages, TA status words, SW_EVENT; last kick -> render" @ 0x3218
"CCu last kick: free the scene if RenderFlags NO_RENDER, then queue the render details for 3D" @ 0x3538
f edm.ta.pick @ 0x37a8
"CCu walk the TA queue (sa15): TACount, SUSPEND, READY, TA TQ / dependency / dst / src syncs, RT data free" @ 0x37a8
f edm.ta.cmd_to_rd @ 0x3d88
"CCu copy CMDTA fields into the render details (flags, syncs, strides, spec object, 3D status) + 3DRegUpdates" @ 0x3d88
f edm.ta.ccb_advance @ 0x3fe0
"CCu TA CCB read offset += CMDTA.ui32Size (& 0xffff); ctx.ta_count--" @ 0x3fe0
f edm.ta.abort @ 0x4060
"CCu RT data aborted or RenderFlags ABORT: complete the TA command without running it" @ 0x4060
f edm.ta.kick @ 0x4748
"CCu program BIF/PB/DPM/TA registers from the CMDTA, then VDM_START" @ 0x4748
"CCu first kick: pulse SOFT_RESET.TA_RESET" @ 0x48e8
"CCu TA register block: CMDTA dwords 142..169 -> TE/VDM/MTE/GSG/BIF_TA_REQ_BASE/DPM registers" @ 0x4ad0
"CCu first kick: bind DPM state/control tables, clear DPM state per RT, clear the OTPM" @ 0x5070
"CCu VDM_START: the TA runs" @ 0x5660
f edm.ta.oom @ 0x5a80
"CCu DPM out of memory: grow the global list or abort the macrotile and start a partial render" @ 0x5a80
f edm.ta.terminated @ 0x6118
"CCu TA_TERMINATE after an out-of-memory abort: partial (SPM) render" @ 0x6118
"CCu partial render: ISP_START_RENDER" @ 0x6d98
f edm.spm.resume @ 0x6df8
"CCu after a partial render: resume the TA (DPM_OUTOFMEM) and mark the RT status for depth reload" @ 0x6df8
f edm.3d.mem_free @ 0x75a0
"CCu DPM_3D_MEM_FREE: clear sa9 bit 5; complete the render if the PBE has finished" @ 0x75a0
f edm.3d.end_render @ 0x75d8
"CCu PIXELBE_END_RENDER: transfer completion or clear sa9 bit 4; complete if no dealloc pending" @ 0x75d8
f edm.isp.vistest @ 0x7668
"CCu ISP breakpoint: accumulate visibility-test results" @ 0x7668
f edm.3d.syncs_done @ 0x7a28
"CCu render done for one RT: dst sync WComp, dependency, TQ and src syncs, 3D status words, RT status |= 1" @ 0x7a28
f edm.3d.scene_done @ 0x7ce8
"CCu last RT: RT data CommonStatus |= 0x10 (free), clear access words, SW_EVENT" @ 0x7ce8
f edm.3d.complete @ 0x7e88
"CCu 3D completion: partial render -> request 0x80000, else syncs, dequeue, reschedule" @ 0x7e88
f edm.3d.schedule @ 0x81a8
"CCu 3D pipe scheduler: transfer queue first, then the 3D render queue" @ 0x81a8
f edm.3d.pick @ 0x82e8
"CCu walk the 3D queue (sa17): render details of each context, dst / TQ / src syncs ready" @ 0x82e8
f edm.3d.kick @ 0x8958
"CCu bind DPM context, load the 3D PB, render-details regs_3d -> registers, background object, ISP_START_RENDER" @ 0x8958
"CCu hw_3d_registers dwords 0..51 -> BIF_3D/ISP/ZLS/TSP/EVENT_PIXEL_PDS/USE filter/TPU registers in order" @ 0x8df8
"CCu background object dwords 0..2 = rd.spec_object[0..1], BgObjPrimPDSUpdateList[RT index]" @ 0x9468
"CCu ISP_START_RENDER: the 3D render runs" @ 0x96d0
f edm.transfer.pick @ 0x96e0
f edm.transfer.complete @ 0xa308
f edm.pipe.resume @ 0xad68
"CCu TA3DCtl dw52--; when 0, DMS_CTRL &= ~0xf" @ 0xad68
f edm.pipe.pause @ 0xade0
"CCu r16=1: quiesce a busy 3D pipe, r16=2: the TA pipe; TA3DCtl dw52++" @ 0xade0
f edm.post_ta @ 0xac90
"CCu TA queue (sa15) non-empty: edm.request(0x20000)" @ 0xac90
f edm.post_3d @ 0xacf0
"CCu transfer (sa13) or 3D (sa17) queue non-empty: edm.request(0x40000)" @ 0xacf0
f pb.ta_thresholds @ 0xb328
"CCu load PB thresholds (TA, ZLS, global, PDS) into the DPM; clear PBFlags bits 0-1" @ 0xb328
f pb.ta_reload @ 0xb3b0
"CCu reload the TA free list from its status registers, DPM_TASK_TA_FREE" @ 0xb3b0
f pb.ta_store @ 0xb468
"CCu TA free list status -> PB desc FreeListHT/Prev, local/global pages" @ 0xb468
f pb.ta_load @ 0xb510
"CCu PB desc -> DPM TA page table base, free list, thresholds, DPM_TASK_TA_FREE" @ 0xb510
f pb.3d_store @ 0xb6a0
"CCu 3D free list status -> PB desc" @ 0xb6a0
f pb.3d_load @ 0xb748
"CCu PB desc -> DPM 3D page table base, free list, DPM_TASK_3D_FREE" @ 0xb748
f pb.init_page_table @ 0xb868
"CCu PBFlags bit 0: reset the free list and link the page table from the PB blocks" @ 0xb868
f dpm.state_store @ 0xba40
"CCu DPM_TASK_STATE=STORE, wait DPM_STATE_STORE" @ 0xba40
f dpm.state_load @ 0xba78
"CCu DPM_TASK_STATE=LOAD, wait DPM_STATE_LOAD" @ 0xba78
f dpm.ta_alloc_pause @ 0xbb28
f dpm.ta_alloc_resume @ 0xbb98
f dpm.ctx_store @ 0xbbc0
"CCu store an RT data's DPM state, control, OTPM and tail pointers (r16 RT data, r17 slot, r18 bank)" @ 0xbbc0
f dpm.ctx_load @ 0xbdf0
"CCu load an RT data's DPM state, control and OTPM" @ 0xbdf0
f bif.map_context @ 0xbf60
"CCu find or assign a BIF directory list for ctx.sPDDevPAddr (r17 0=TA, 1=3D); NEWCONTEXT -> cache invalidate" @ 0xbf60
f bif.set_index @ 0xc260
"CCu BIF_BANK0.INDEX_TA (p1 clear) or INDEX_3D (p1 set) = r19" @ 0xc260
f edm.power.done @ 0xc2c0
"CCu sa12 &= ~r16; when idle sa11 = HostCtl dw7" @ 0xc2c0
f bif.edm_index @ 0xc320
"CCu BIF_BANK0.INDEX_EDM = TA's (0), 3D's (1) or the EDM's own list 15 (2)" @ 0xc320
f dpm.free_scene @ 0xc3e0
"CCu free a scene's parameter memory without rendering: store/load DPM, DPM_FREE_CONTEXT, TPC clear" @ 0xc3e0
f edm.3d.queue_render @ 0xca10
"CCu move render details from ctx partial list to complete list; queue ctx on the 3D queue (sa17/sa18)" @ 0xca10
f edm.task.end @ 0xcba0
"CCu flush caches and end the task" @ 0xcba0
