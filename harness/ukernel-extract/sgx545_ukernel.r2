f edm.ta @ 0x8e8
af @ 0x8e8
afn edm.ta @ 0x8e8
CCu SGXMKIF_CMD_TA handler (host kick address 0x11d) @ 0x8e8
f edm.transfer @ 0x818
af @ 0x818
afn edm.transfer @ 0x818
CCu SGXMKIF_CMD_TRANSFER handler (host kick address 0x103) @ 0x818
f edm.power @ 0x6a0
af @ 0x6a0
afn edm.power @ 0x6a0
CCu SGXMKIF_CMD_POWER handler (host kick address 0xd4) @ 0x6a0
f edm.contextsuspend @ 0x758
af @ 0x758
afn edm.contextsuspend @ 0x758
CCu SGXMKIF_CMD_CONTEXTSUSPEND handler (host kick address 0xeb) @ 0x758
f edm.cleanup @ 0x9d0
af @ 0x9d0
afn edm.cleanup @ 0x9d0
CCu SGXMKIF_CMD_CLEANUP handler (host kick address 0x13a) @ 0x9d0
f edm.getmiscinfo @ 0xb98
af @ 0xb98
afn edm.getmiscinfo @ 0xb98
CCu SGXMKIF_CMD_GETMISCINFO handler (host kick address 0x173) @ 0xb98
f edm.process_queues @ 0xab0
af @ 0xab0
afn edm.process_queues @ 0xab0
CCu SGXMKIF_CMD_PROCESS_QUEUES handler (host kick address 0x156) @ 0xab0
f edm.sethwperfstatus @ 0xab8
af @ 0xab8
afn edm.sethwperfstatus @ 0xab8
CCu SGXMKIF_CMD_SETHWPERFSTATUS handler (host kick address 0x157) @ 0xab8
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
CCu read EUR_CR_DPM_PAGE @ 0x140
CCu read EUR_CR_DPM_PAGE_STATUS @ 0x3b0
CCu read cr_cache_ctrl @ 0x590
CCu write cr_cache_ctrl @ 0x5a8
CCu read EUR_CR_EVENT_STATUS @ 0x5b0
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0x5d8
CCu write cr_dpm_outofmem @ 0xb40
CCu read EUR_CR_CORE_REVISION @ 0xba0
CCu read EUR_CR_CORE_ID @ 0xba8
CCu write EUR_CR_EVENT_OTHER_PDS_EXEC @ 0xe48
CCu write EUR_CR_EVENT_OTHER_PDS_DATA @ 0xe58
CCu write cr_perf @ 0xf68
CCu write cr_emu_cycle_count @ 0xf70
CCu write cr_emu_cycle_count @ 0xf78
CCu write cr_perf @ 0xfb8
CCu write EUR_CR_EVENT_TIMER @ 0xfe0
CCu write EUR_CR_EVENT_STATUS @ 0x1cc0
CCu write EUR_CR_EVENT_STATUS @ 0x26a8
CCu read EUR_CR_USE0_SERV_EVENT @ 0x27b8
CCu write EUR_CR_EVENT_TIMER @ 0x2850
CCu read EUR_CR_DPM_PAGE_STATUS @ 0x2d68
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0x2df8
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0x2f68
CCu write cr_dpm_task_state @ 0x3140
CCu write cr_dpm_task_control @ 0x3148
CCu write cr_mte_otpm_op @ 0x3150
CCu write cr_te_tpc_control @ 0x3160
CCu read EUR_CR_EVENT_STATUS @ 0x3170
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0x31a0
CCu read EUR_CR_DPM_ZLS_PAGE_THRESHOLD @ 0x31c0
CCu write EUR_CR_DPM_ZLS_PAGE_THRESHOLD @ 0x31d8
CCu write EUR_CR_DPM_TA_GLOBAL_LIST @ 0x31f8
CCu read EUR_CR_DPM_PAGE_STATUS @ 0x32b8
CCu write EUR_CR_EVENT_STATUS @ 0x3360
CCu write cr_te_tpc_control @ 0x33f8
CCu read EUR_CR_EVENT_STATUS @ 0x3400
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0x3428
CCu read cr_te_state @ 0x34c0
CCu write EUR_CR_EVENT_STATUS @ 0x4368
CCu read EUR_CR_DPM_STATE_CONTEXT_ID @ 0x47f8
CCu write EUR_CR_SOFT_RESET @ 0x48e8
CCu write EUR_CR_SOFT_RESET @ 0x48f0
CCu write cr_te_aa @ 0x4af0
CCu write cr_te_mtile_1 @ 0x4af8
CCu write cr_te_mtile_2 @ 0x4b00
CCu write cr_te_screen @ 0x4b08
CCu write cr_te_mtile @ 0x4b10
CCu write cr_te_psg @ 0x4b18
CCu write cr_vdm_ctrl_stream_base @ 0x4b20
CCu write EUR_CR_MTE_CTRL @ 0x4b28
CCu write cr_te_psgregion_base @ 0x4b40
CCu write cr_te_tpc_base @ 0x4b58
CCu write cr_mte_wcompare @ 0x4b70
CCu write cr_mte_wclamp @ 0x4b78
CCu write cr_mte_screen @ 0x4b80
CCu write cr_use_ld_redirect @ 0x4b88
CCu write cr_use_st_range @ 0x4b90
CCu write EUR_CR_BIF_TA_REQ_BASE @ 0x4b98
CCu write cr_gsg @ 0x4bb0
CCu write cr_gsg_base @ 0x4bb8
CCu write cr_gsg_wrap @ 0x4bc0
CCu write cr_gsg_store @ 0x4bc8
CCu write cr_mte_1st_phase_complex_base @ 0x4bd0
CCu write cr_mte_1st_phase_complex_buffer @ 0x4bd8
CCu write cr_vdm_vtxbuf_wrptr_pds_prog @ 0x4be0
CCu write cr_vdm_scword_cp_pds_prog @ 0x4be8
CCu write cr_vdm_itp_pds_prog @ 0x4bf0
CCu write cr_dpm_ta_render_target_max @ 0x4c08
CCu write cr_te_tpc @ 0x4c10
CCu write cr_mte_multisample_ctl @ 0x4c28
CCu read EUR_CR_DMS_CTRL @ 0x4c30
CCu write EUR_CR_DMS_CTRL @ 0x4c60
CCu read cr_cache_ctrl @ 0x4cd0
CCu write cr_cache_ctrl @ 0x4ce8
CCu read EUR_CR_EVENT_STATUS @ 0x4cf8
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0x4d20
CCu write EUR_CR_PDS_INV0 @ 0x4d48
CCu write EUR_CR_PDS_INV_CSC @ 0x4d50
CCu read cr_pds_cache_status @ 0x4d58
CCu write cr_pds_cache_host_clear @ 0x4d80
CCu write cr_use_cache @ 0x4da8
CCu read EUR_CR_DPM_STATE_CONTEXT_ID @ 0x4e40
CCu read EUR_CR_DPM_STATE_CONTEXT_ID @ 0x4eb0
CCu write EUR_CR_DPM_STATE_CONTEXT_ID @ 0x4ed0
CCu read EUR_CR_DPM_STATE_CONTEXT_ID @ 0x5070
CCu write EUR_CR_DPM_STATE_CONTEXT_ID @ 0x5090
CCu write cr_dpm_state_table_context_0_base @ 0x50c0
CCu write cr_dpm_state_table_context_1_base @ 0x50d0
CCu write cr_dpm_control_table_base @ 0x50e0
CCu write cr_dpm_task_state @ 0x5118
CCu write cr_dpm_task_control @ 0x5120
CCu read EUR_CR_EVENT_STATUS @ 0x5128
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0x5150
CCu read cr_dpm_state_rentarget @ 0x5178
CCu write cr_dpm_task_state @ 0x51c0
CCu write cr_dpm_task_control @ 0x51c8
CCu read EUR_CR_EVENT_STATUS @ 0x51d8
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0x5200
CCu read cr_dpm_state_rentarget @ 0x5210
CCu write cr_dpm_task_state @ 0x5248
CCu write cr_dpm_task_control @ 0x5250
CCu write cr_mte_otpm_csm_base @ 0x5270
CCu write cr_mte_otpm_op @ 0x5278
CCu read EUR_CR_EVENT_STATUS @ 0x5288
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0x52b0
CCu read EUR_CR_EVENT_STATUS2 @ 0x52d8
CCu write EUR_CR_EVENT_HOST_CLEAR2 @ 0x5300
CCu write cr_te_state @ 0x5338
CCu read cr_te_psg @ 0x5348
CCu write cr_te_psg @ 0x5370
CCu read EUR_CR_DPM_PAGE_STATUS @ 0x5580
CCu write cr_vdm_start @ 0x5660
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0x56a0
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0x5708
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0x57c0
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0x57f8
CCu read EUR_CR_DPM_ABORT_STATUS_MTILE @ 0x5860
CCu read EUR_CR_DPM_REQUESTING @ 0x5c80
CCu read EUR_CR_DPM_GLOBAL_PAGE_STATUS @ 0x5cb8
CCu read EUR_CR_DPM_TA_GLOBAL_LIST @ 0x5cc0
CCu write EUR_CR_DPM_TA_GLOBAL_LIST @ 0x5d00
CCu write cr_dpm_outofmem @ 0x5d58
CCu write EUR_CR_DPM_TA_GLOBAL_LIST @ 0x5da8
CCu read EUR_CR_DPM_PAGE_STATUS @ 0x5e30
CCu read EUR_CR_DPM_GLOBAL_PAGE_STATUS @ 0x5e68
CCu read cr_te_psg @ 0x5eb0
CCu write cr_te_psg @ 0x5ec8
CCu read EUR_CR_DPM_ZLS_PAGE_THRESHOLD @ 0x5ee8
CCu write EUR_CR_DPM_ZLS_PAGE_THRESHOLD @ 0x5f08
CCu read EUR_CR_DPM_TA_GLOBAL_LIST @ 0x5f18
CCu write EUR_CR_DPM_TA_GLOBAL_LIST @ 0x5f48
CCu write cr_dpm_outofmem @ 0x5fa0
CCu write cr_mte_otpm_op @ 0x5fb8
CCu read EUR_CR_EVENT_STATUS @ 0x5fc8
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0x5ff0
CCu read EUR_CR_DPM_ABORT_STATUS_MTILE @ 0x6000
CCu read EUR_CR_DPM_GLOBAL_PAGE_STATUS @ 0x6050
CCu write cr_dpm_outofmem @ 0x6090
CCu read EUR_CR_DPM_PAGE_STATUS @ 0x6098
CCu read EUR_CR_DMS_CTRL @ 0x60d0
CCu write EUR_CR_DMS_CTRL @ 0x60e8
CCu read EUR_CR_DPM_STATE_CONTEXT_ID @ 0x6200
CCu write EUR_CR_DPM_STATE_CONTEXT_ID @ 0x6330
CCu write cr_dpm_state_table_context_0_base @ 0x6358
CCu write cr_dpm_state_table_context_1_base @ 0x6368
CCu read EUR_CR_BIF_BANK_SET @ 0x6370
CCu write EUR_CR_BIF_BANK_SET @ 0x6388
CCu write cr_dpm_lss_partial_context @ 0x63a8
CCu read EUR_CR_DPM_STATE_CONTEXT_ID @ 0x63e8
CCu write EUR_CR_DPM_STATE_CONTEXT_ID @ 0x6400
CCu read cr_dpm_state_table_context_0_base @ 0x6418
CCu read cr_dpm_state_table_context_1_base @ 0x6428
CCu write cr_dpm_state_table_context_0_base @ 0x6448
CCu write cr_dpm_state_table_context_1_base @ 0x6458
CCu read EUR_CR_DPM_STATE_CONTEXT_ID @ 0x6478
CCu write EUR_CR_DPM_STATE_CONTEXT_ID @ 0x6490
CCu write cr_dpm_lss_partial_context @ 0x6498
CCu write cr_dpm_3d_deallocate @ 0x65d8
CCu write cr_dpm_partial_render @ 0x65f0
CCu write cr_isp_vistest_ctrl @ 0x6610
CCu write cr_isp_rend_box_1 @ 0x6620
CCu write cr_isp_rend_box_2 @ 0x6628
CCu write cr_isp_render @ 0x6630
CCu write EUR_CR_BIF_3D_REQ_BASE @ 0x6640
CCu write EUR_CR_BIF_ZLS_REQ_BASE @ 0x6648
CCu write cr_pixelbe @ 0x6650
CCu write cr_isp_ipf_misc @ 0x6658
CCu write cr_3d_aa_mode @ 0x6660
CCu write cr_isp_dbias_0 @ 0x6668
CCu write cr_isp_dbias_1 @ 0x6670
CCu write cr_isp_dbias_2 @ 0x6678
CCu write cr_isp_rgn_base @ 0x6690
CCu write cr_double_pixel_partitions @ 0x66a8
CCu write cr_isp2_zls_extz_rgn_base @ 0x66c0
CCu write cr_isp_zls_ctl @ 0x66d8
CCu write cr_isp_zload_base @ 0x66e0
CCu write cr_isp_zstore_base @ 0x66e8
CCu write cr_isp_stencil_load_base @ 0x66f0
CCu write cr_isp_stencil_store_base @ 0x66f8
CCu write cr_isp_bgobj_depth @ 0x6700
CCu write cr_isp_bgobj @ 0x6708
CCu write cr_isp_bgobj_tag @ 0x6710
CCu write cr_isp_multisample_ctl_2 @ 0x6728
CCu write cr_isp_multisample_ctl @ 0x6740
CCu write cr_isp_tag_ctrl @ 0x6748
CCu write cr_tsp_config @ 0x6750
CCu write cr_event_pixel_pds_exec @ 0x6758
CCu write cr_event_pixel_pds_data @ 0x6760
CCu write cr_event_pixel_pds_info @ 0x6768
CCu write cr_isp_fpu_ctrl @ 0x6770
CCu write cr_use_filter_0_left @ 0x6778
CCu write cr_use_filter_0_right @ 0x6790
CCu write cr_use_filter_0_extra @ 0x67a8
CCu write cr_use_filter_1_left @ 0x67b0
CCu write cr_use_filter_1_right @ 0x67b8
CCu write cr_use_filter_1_extra @ 0x67c0
CCu write cr_use_filter_2_left @ 0x67c8
CCu write cr_use_filter_2_right @ 0x67d0
CCu write cr_use_filter_2_extra @ 0x67d8
CCu write cr_use_filter_table @ 0x67e0
CCu write cr_tpu_yuv_00 @ 0x67f8
CCu write cr_tpu_yuv_01 @ 0x6800
CCu write cr_tpu_yuv_02 @ 0x6808
CCu write cr_tpu_yuv_03 @ 0x6810
CCu write cr_tpu_yuv_10 @ 0x6818
CCu write cr_tpu_yuv_11 @ 0x6820
CCu write cr_tpu_yuv_12 @ 0x6828
CCu write cr_tpu_yuv_13 @ 0x6830
CCu write cr_tpu_yuv_04 @ 0x6848
CCu write cr_tpu_yuv_14 @ 0x6850
CCu write cr_tpu_yuv_scale @ 0x6868
CCu write cr_tpu_luma_0 @ 0x6880
CCu write cr_tpu_luma_1 @ 0x6888
CCu write cr_tpu_luma_2 @ 0x6890
CCu write cr_isp_depth_sort @ 0x68a8
CCu write cr_dpm_3d_render_target_index @ 0x68c0
CCu read cr_isp_rgn_base @ 0x68d8
CCu write cr_isp_rgn_base @ 0x6900
CCu read cr_isp_zload_base @ 0x6910
CCu read cr_isp_zstore_base @ 0x6918
CCu write cr_isp_zload_base @ 0x6938
CCu write cr_isp_zstore_base @ 0x6940
CCu read cr_isp_stencil_load_base @ 0x6950
CCu read cr_isp_stencil_store_base @ 0x6958
CCu write cr_isp_stencil_load_base @ 0x6978
CCu write cr_isp_stencil_store_base @ 0x6980
CCu read cr_isp_zls_ctl @ 0x6988
CCu write cr_isp_zls_ctl @ 0x6a20
CCu write cr_isp_zls_ctl @ 0x6ab8
CCu read cr_isp_ipf_misc @ 0x6ad0
CCu write cr_isp_ipf_misc @ 0x6ae8
CCu read cr_isp_zls_ctl @ 0x6b18
CCu write cr_isp_zls_ctl @ 0x6b30
CCu read EUR_CR_DMS_CTRL @ 0x6b38
CCu write EUR_CR_DMS_CTRL @ 0x6b68
CCu write cr_use_cache @ 0x6b80
CCu write EUR_CR_PDS_INV1 @ 0x6b90
CCu write EUR_CR_PDS_INV_CSC @ 0x6b98
CCu write cr_tsp_parameter_cache @ 0x6ba0
CCu read cr_pds_cache_status @ 0x6ba8
CCu write cr_pds_cache_host_clear @ 0x6bd0
CCu read cr_isp_rgn_base @ 0x6c28
CCu write cr_isp_rgn_base @ 0x6c40
CCu write cr_isp2_zls_extz_rgn_base @ 0x6c48
CCu read cr_isp_rgn_base @ 0x6c58
CCu write cr_isp2_zls_extz_rgn_base @ 0x6c68
CCu read EUR_CR_DPM_PAGE @ 0x6d10
CCu write cr_isp_start_render @ 0x6d98
CCu read EUR_CR_DPM_PAGE_STATUS @ 0x6e68
CCu read EUR_CR_DPM_GLOBAL_PAGE_STATUS @ 0x6eb0
CCu read EUR_CR_DMS_CTRL @ 0x6ee8
CCu write EUR_CR_DMS_CTRL @ 0x6f00
CCu write cr_dpm_lss_partial_context @ 0x6f18
CCu write cr_dpm_task_control @ 0x6f20
CCu write cr_dpm_partial_render @ 0x6f28
CCu read EUR_CR_EVENT_STATUS @ 0x6f38
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0x6f68
CCu write cr_dpm_lss_partial_context @ 0x6f70
CCu write cr_dpm_outofmem @ 0x6fb8
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0x70f0
CCu read EUR_CR_DPM_PAGE @ 0x7138
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0x72b8
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0x72f0
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0x7468
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0x74a0
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0x74d8
CCu read EUR_CR_CLKGATECTL @ 0x75f0
CCu read EUR_CR_CLKGATESTATUS @ 0x7618
CCu read EUR_CR_ISP_STATUS1 @ 0x76c0
CCu read EUR_CR_BIF_3D_REQ_BASE @ 0x76c8
CCu read cr_isp_vistest_visible_0 @ 0x7768
CCu read cr_isp_vistest_visible_1 @ 0x7770
CCu read cr_isp_vistest_visible_2 @ 0x7780
CCu read cr_isp_vistest_visible_3 @ 0x7788
CCu read cr_isp_vistest_visible_4 @ 0x7808
CCu read cr_isp_vistest_visible_5 @ 0x7810
CCu read cr_isp_vistest_visible_6 @ 0x7890
CCu read cr_isp_vistest_visible_7 @ 0x7898
CCu write cr_isp_vistest_ctrl @ 0x79b8
CCu read EUR_CR_ISP_STATUS2 @ 0x79c8
CCu read cr_isp_status_3 @ 0x79d0
CCu read EUR_CR_ISP_STATUS1 @ 0x79d8
CCu write cr_isp_rgn_base @ 0x7a00
CCu write cr_isp_start @ 0x7a08
CCu write cr_isp_break @ 0x7a18
CCu write EUR_CR_EVENT_STATUS @ 0x7e78
CCu read EUR_CR_DPM_STATE_CONTEXT_ID @ 0x8a48
CCu read EUR_CR_DPM_STATE_CONTEXT_ID @ 0x8ab0
CCu write EUR_CR_DPM_STATE_CONTEXT_ID @ 0x8ad0
CCu write cr_dpm_3d_deallocate @ 0x8da8
CCu write cr_dpm_3d_deallocate @ 0x8db8
CCu write cr_isp_vistest_ctrl @ 0x8df0
CCu write cr_isp_rend_box_1 @ 0x8e00
CCu write cr_isp_rend_box_2 @ 0x8e08
CCu write cr_isp_render @ 0x8e10
CCu write EUR_CR_BIF_3D_REQ_BASE @ 0x8e20
CCu write EUR_CR_BIF_ZLS_REQ_BASE @ 0x8e28
CCu write cr_pixelbe @ 0x8e30
CCu write cr_isp_ipf_misc @ 0x8e38
CCu write cr_3d_aa_mode @ 0x8e40
CCu write cr_isp_dbias_0 @ 0x8e48
CCu write cr_isp_dbias_1 @ 0x8e50
CCu write cr_isp_dbias_2 @ 0x8e58
CCu write cr_isp_rgn_base @ 0x8e70
CCu write cr_double_pixel_partitions @ 0x8e88
CCu write cr_isp2_zls_extz_rgn_base @ 0x8ea0
CCu write cr_isp_zls_ctl @ 0x8eb8
CCu write cr_isp_zload_base @ 0x8ec0
CCu write cr_isp_zstore_base @ 0x8ec8
CCu write cr_isp_stencil_load_base @ 0x8ed0
CCu write cr_isp_stencil_store_base @ 0x8ed8
CCu write cr_isp_bgobj_depth @ 0x8ee0
CCu write cr_isp_bgobj @ 0x8ee8
CCu write cr_isp_bgobj_tag @ 0x8ef0
CCu write cr_isp_multisample_ctl_2 @ 0x8f08
CCu write cr_isp_multisample_ctl @ 0x8f20
CCu write cr_isp_tag_ctrl @ 0x8f28
CCu write cr_tsp_config @ 0x8f30
CCu write cr_event_pixel_pds_exec @ 0x8f38
CCu write cr_event_pixel_pds_data @ 0x8f40
CCu write cr_event_pixel_pds_info @ 0x8f48
CCu write cr_isp_fpu_ctrl @ 0x8f50
CCu write cr_use_filter_0_left @ 0x8f58
CCu write cr_use_filter_0_right @ 0x8f70
CCu write cr_use_filter_0_extra @ 0x8f88
CCu write cr_use_filter_1_left @ 0x8f90
CCu write cr_use_filter_1_right @ 0x8f98
CCu write cr_use_filter_1_extra @ 0x8fa0
CCu write cr_use_filter_2_left @ 0x8fa8
CCu write cr_use_filter_2_right @ 0x8fb0
CCu write cr_use_filter_2_extra @ 0x8fb8
CCu write cr_use_filter_table @ 0x8fc0
CCu write cr_tpu_yuv_00 @ 0x8fd8
CCu write cr_tpu_yuv_01 @ 0x8fe0
CCu write cr_tpu_yuv_02 @ 0x8fe8
CCu write cr_tpu_yuv_03 @ 0x8ff0
CCu write cr_tpu_yuv_10 @ 0x8ff8
CCu write cr_tpu_yuv_11 @ 0x9000
CCu write cr_tpu_yuv_12 @ 0x9008
CCu write cr_tpu_yuv_13 @ 0x9010
CCu write cr_tpu_yuv_04 @ 0x9028
CCu write cr_tpu_yuv_14 @ 0x9030
CCu write cr_tpu_yuv_scale @ 0x9048
CCu write cr_tpu_luma_0 @ 0x9060
CCu write cr_tpu_luma_1 @ 0x9068
CCu write cr_tpu_luma_2 @ 0x9070
CCu write cr_isp_depth_sort @ 0x9088
CCu write cr_dpm_3d_render_target_index @ 0x9090
CCu read cr_isp_rgn_base @ 0x90b0
CCu write cr_isp_rgn_base @ 0x90c8
CCu read cr_isp_zload_base @ 0x90d8
CCu read cr_isp_zstore_base @ 0x90e0
CCu write cr_isp_zload_base @ 0x9100
CCu write cr_isp_zstore_base @ 0x9108
CCu read cr_isp_stencil_load_base @ 0x9118
CCu read cr_isp_stencil_store_base @ 0x9120
CCu write cr_isp_stencil_load_base @ 0x9140
CCu write cr_isp_stencil_store_base @ 0x9148
CCu read cr_isp_zls_ctl @ 0x9200
CCu write cr_isp_zls_ctl @ 0x9218
CCu read EUR_CR_DMS_CTRL @ 0x9220
CCu write EUR_CR_DMS_CTRL @ 0x9250
CCu write cr_dpm_partial_render @ 0x9258
CCu read cr_cache_ctrl @ 0x9260
CCu write cr_cache_ctrl @ 0x9278
CCu read EUR_CR_EVENT_STATUS @ 0x9288
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0x92b0
CCu write cr_tsp_parameter_cache @ 0x92c0
CCu write cr_use_cache @ 0x92d8
CCu write EUR_CR_PDS_INV1 @ 0x92e8
CCu write EUR_CR_PDS_INV_CSC @ 0x92f0
CCu read cr_pds_cache_status @ 0x92f8
CCu write cr_pds_cache_host_clear @ 0x9320
CCu read cr_isp_zls_ctl @ 0x9380
CCu write cr_isp_zls_ctl @ 0x93e8
CCu read cr_isp_zls_ctl @ 0x93f8
CCu write cr_isp_zls_ctl @ 0x9440
CCu read EUR_CR_DPM_PAGE @ 0x9648
CCu write cr_isp_start_render @ 0x96d0
CCu read EUR_CR_DMS_CTRL @ 0x9d10
CCu write EUR_CR_DMS_CTRL @ 0x9d40
CCu write cr_dpm_3d_deallocate @ 0x9e20
CCu write cr_isp_zls_ctl @ 0x9e28
CCu write cr_isp_bgobj_tag @ 0x9e38
CCu write cr_isp_bgobj @ 0x9e40
CCu write cr_isp_bgobj_depth @ 0x9e48
CCu write cr_isp_render @ 0x9e50
CCu write cr_isp_rgn_base @ 0x9e58
CCu write cr_isp_ipf_misc @ 0x9e60
CCu write EUR_CR_BIF_3D_REQ_BASE @ 0x9e78
CCu write cr_3d_aa_mode @ 0x9e80
CCu write cr_isp_multisample_ctl @ 0x9e88
CCu write cr_event_pixel_pds_exec @ 0x9e90
CCu write cr_event_pixel_pds_data @ 0x9e98
CCu write cr_event_pixel_pds_info @ 0x9ea0
CCu write cr_isp_rend_box_1 @ 0x9ec0
CCu write cr_isp_rend_box_2 @ 0x9ec8
CCu write cr_pixelbe @ 0x9ed0
CCu write EUR_CR_PDS_EXEC_BASE @ 0x9ed8
CCu write cr_double_pixel_partitions @ 0x9ee8
CCu write cr_tpu_yuv_00 @ 0x9f18
CCu write cr_tpu_yuv_01 @ 0x9f20
CCu write cr_tpu_yuv_02 @ 0x9f28
CCu write cr_tpu_yuv_03 @ 0x9f30
CCu write cr_tpu_yuv_04 @ 0x9f38
CCu write cr_tpu_yuv_10 @ 0x9f48
CCu write cr_tpu_yuv_11 @ 0x9f50
CCu write cr_tpu_yuv_12 @ 0x9f58
CCu write cr_tpu_yuv_13 @ 0x9f60
CCu write cr_tpu_yuv_14 @ 0x9f68
CCu write cr_tpu_yuv_scale @ 0x9f70
CCu write cr_tpu_luma_0 @ 0x9fc0
CCu write cr_tpu_luma_1 @ 0x9fc8
CCu write cr_tpu_luma_2 @ 0x9fd0
CCu write cr_use_filter_table @ 0xa010
CCu write cr_use_filter_0_left @ 0xa018
CCu write cr_use_filter_0_right @ 0xa020
CCu write cr_use_filter_0_extra @ 0xa028
CCu write cr_use_filter_1_left @ 0xa030
CCu write cr_use_filter_1_right @ 0xa038
CCu write cr_use_filter_1_extra @ 0xa040
CCu write cr_use_filter_2_left @ 0xa048
CCu write cr_use_filter_2_right @ 0xa058
CCu write cr_use_filter_2_extra @ 0xa060
CCu write EUR_CR_PDS_INV1 @ 0xa068
CCu write EUR_CR_PDS_INV_CSC @ 0xa070
CCu write cr_tsp_parameter_cache @ 0xa078
CCu read cr_cache_ctrl @ 0xa080
CCu write cr_cache_ctrl @ 0xa098
CCu read EUR_CR_EVENT_STATUS @ 0xa0a0
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0xa0c8
CCu write cr_use_cache @ 0xa0e8
CCu read cr_pds_cache_status @ 0xa0f8
CCu write cr_pds_cache_host_clear @ 0xa120
CCu write cr_isp_start_render @ 0xa2f8
CCu write EUR_CR_EVENT_STATUS @ 0xa7c0
CCu write cr_dpm_outofmem @ 0xabf8
CCu read EUR_CR_BIF_CTRL @ 0xac58
CCu write EUR_CR_BIF_CTRL @ 0xac78
CCu write EUR_CR_BIF_CTRL @ 0xac80
CCu read EUR_CR_DMS_CTRL @ 0xadb8
CCu write EUR_CR_DMS_CTRL @ 0xadd0
CCu read EUR_CR_DMS_CTRL @ 0xae58
CCu write EUR_CR_DMS_CTRL @ 0xae70
CCu read cr_mte_sig_1 @ 0xae90
CCu read cr_mte_sig_1 @ 0xaeb8
CCu read cr_te_diag_2 @ 0xaed8
CCu read cr_te_diag_5 @ 0xaee0
CCu read cr_te_diag_2 @ 0xaf10
CCu read cr_te_diag_5 @ 0xaf18
CCu read cr_isp_pipe_0_sig_3 @ 0xaf48
CCu read cr_isp_pipe_0_sig_4 @ 0xaf50
CCu read cr_isp_pipe_1_sig_3 @ 0xaf68
CCu read cr_isp_pipe_1_sig_4 @ 0xaf70
CCu read cr_isp_pipe_0_sig_3 @ 0xafb0
CCu read cr_isp_pipe_0_sig_4 @ 0xafb8
CCu read cr_isp_pipe_1_sig_3 @ 0xafd0
CCu read cr_isp_pipe_1_sig_4 @ 0xafe8
CCu read cr_pixelbe_sig_01 @ 0xb010
CCu read cr_pixelbe_sig_02 @ 0xb018
CCu read cr_pixelbe_sig_01 @ 0xb048
CCu read cr_pixelbe_sig_02 @ 0xb050
CCu read EUR_CR_USE0_SERV_VERTEX @ 0xb0a0
CCu read EUR_CR_USE1_SERV_VERTEX @ 0xb0a8
CCu read cr_use_2_serv_vertex @ 0xb0c0
CCu read cr_use_3_serv_vertex @ 0xb0d8
CCu read EUR_CR_USE0_SERV_PIXEL @ 0xb0f8
CCu read EUR_CR_USE1_SERV_PIXEL @ 0xb100
CCu read cr_use_2_serv_pixel @ 0xb118
CCu read cr_use_3_serv_pixel @ 0xb130
CCu read EUR_CR_USE1_DM_SLOT @ 0xb1a8
CCu read cr_use_2_dm_slot @ 0xb1c8
CCu read cr_use_3_dm_slot @ 0xb1e8
CCu read EUR_CR_USE0_DM_SLOT @ 0xb270
CCu read EUR_CR_DMS_CTRL @ 0xb2e8
CCu write EUR_CR_DMS_CTRL @ 0xb300
CCu write cr_dpm_ta_page_threshold @ 0xb348
CCu write EUR_CR_DPM_TA_GLOBAL_LIST @ 0xb368
CCu write cr_dpm_pds_page_threshold @ 0xb370
CCu write EUR_CR_DPM_ZLS_PAGE_THRESHOLD @ 0xb378
CCu read EUR_CR_DPM_TA_ALLOC_FREE_LIST_STATUS1 @ 0xb3b0
CCu read EUR_CR_DPM_TA_ALLOC_FREE_LIST_STATUS2 @ 0xb3b8
CCu read EUR_CR_DPM_PAGE_STATUS @ 0xb3c0
CCu read EUR_CR_DPM_GLOBAL_PAGE_STATUS @ 0xb3e0
CCu write cr_dpm_ta_alloc_free_list @ 0xb400
CCu write cr_dpm_ta_alloc @ 0xb410
CCu write cr_dpm_start_of_context_pages_allocated @ 0xb428
CCu write cr_dpm_task_ta_free @ 0xb430
CCu read EUR_CR_EVENT_STATUS2 @ 0xb438
CCu write EUR_CR_EVENT_HOST_CLEAR2 @ 0xb458
CCu read EUR_CR_DPM_TA_ALLOC_FREE_LIST_STATUS1 @ 0xb470
CCu read EUR_CR_DPM_TA_ALLOC_FREE_LIST_STATUS2 @ 0xb478
CCu read EUR_CR_DPM_PAGE_STATUS @ 0xb4a8
CCu read EUR_CR_DPM_GLOBAL_PAGE_STATUS @ 0xb4b0
CCu write cr_dpm_ta_alloc_page_table_base @ 0xb538
CCu write cr_dpm_ta_alloc_free_list @ 0xb550
CCu write cr_dpm_ta_alloc @ 0xb568
CCu write cr_dpm_start_of_context_pages_allocated @ 0xb598
CCu write cr_dpm_ta_page_threshold @ 0xb5b0
CCu write cr_dpm_pds_page_threshold @ 0xb5c8
CCu write EUR_CR_DPM_ZLS_PAGE_THRESHOLD @ 0xb5e0
CCu write EUR_CR_DPM_TA_GLOBAL_LIST @ 0xb608
CCu write EUR_CR_DPM_CONTEXT_PB_BASE @ 0xb660
CCu write cr_dpm_task_ta_free @ 0xb668
CCu read EUR_CR_EVENT_STATUS2 @ 0xb670
CCu write EUR_CR_EVENT_HOST_CLEAR2 @ 0xb690
CCu read EUR_CR_DPM_3D_FREE_LIST_STATUS1 @ 0xb6a8
CCu read EUR_CR_DPM_3D_FREE_LIST_STATUS2 @ 0xb6b0
CCu read EUR_CR_DPM_PAGE @ 0xb6e0
CCu read cr_dpm_global_page @ 0xb6e8
CCu write cr_dpm_3d_page_table_base @ 0xb770
CCu write cr_dpm_3d_free_list @ 0xb788
CCu write cr_dpm_3d @ 0xb7a0
CCu write cr_dpm_start_of_3d_context_pages_allocated @ 0xb7d8
CCu write EUR_CR_DPM_CONTEXT_PB_BASE @ 0xb828
CCu write cr_dpm_task_3d_free @ 0xb830
CCu read EUR_CR_EVENT_STATUS2 @ 0xb838
CCu write EUR_CR_EVENT_HOST_CLEAR2 @ 0xb858
CCu write cr_dpm_task_state @ 0xba08
CCu read EUR_CR_EVENT_STATUS @ 0xba10
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0xba30
CCu write cr_dpm_task_state @ 0xba40
CCu read EUR_CR_EVENT_STATUS @ 0xba48
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0xba68
CCu write cr_dpm_task_state @ 0xba78
CCu read EUR_CR_EVENT_STATUS @ 0xba80
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0xbaa0
CCu read EUR_CR_DMS_CTRL @ 0xbb28
CCu write EUR_CR_DMS_CTRL @ 0xbb40
CCu read EUR_CR_DPM_TA_ALLOC_FREE_LIST_STATUS1 @ 0xbb48
CCu read EUR_CR_DPM_TA_ALLOC_FREE_LIST_STATUS1 @ 0xbb70
CCu read EUR_CR_DMS_CTRL @ 0xbb98
CCu write EUR_CR_DMS_CTRL @ 0xbbb0
CCu read EUR_CR_BIF_BANK_SET @ 0xbbc0
CCu write EUR_CR_BIF_BANK_SET @ 0xbbe0
CCu read EUR_CR_DPM_STATE_CONTEXT_ID @ 0xbbf8
CCu write EUR_CR_DPM_STATE_CONTEXT_ID @ 0xbc18
CCu write cr_dpm_state_table_context_0_base @ 0xbc40
CCu write cr_dpm_state_table_context_1_base @ 0xbc50
CCu write cr_dpm_task_state @ 0xbc58
CCu read EUR_CR_EVENT_STATUS @ 0xbc60
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0xbc80
CCu write cr_dpm_control_table_base @ 0xbcb8
CCu write cr_dpm_task_control @ 0xbcc0
CCu write cr_mte_otpm_csm_base @ 0xbce8
CCu write cr_mte_otpm_op @ 0xbcf0
CCu write cr_te_tpc_base @ 0xbd18
CCu write cr_te_tpc_control @ 0xbd28
CCu read EUR_CR_EVENT_STATUS @ 0xbd38
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0xbd68
CCu read EUR_CR_EVENT_STATUS2 @ 0xbd78
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0xbda0
CCu write cr_dpm_task_control @ 0xbda8
CCu read EUR_CR_EVENT_STATUS @ 0xbdb8
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0xbde0
CCu read EUR_CR_BIF_BANK_SET @ 0xbdf0
CCu write EUR_CR_BIF_BANK_SET @ 0xbe10
CCu read EUR_CR_DPM_STATE_CONTEXT_ID @ 0xbe28
CCu write EUR_CR_DPM_STATE_CONTEXT_ID @ 0xbe48
CCu write cr_dpm_state_table_context_0_base @ 0xbe70
CCu write cr_dpm_state_table_context_1_base @ 0xbe80
CCu write cr_dpm_task_state @ 0xbe88
CCu read EUR_CR_EVENT_STATUS @ 0xbe90
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0xbeb0
CCu write cr_dpm_control_table_base @ 0xbed8
CCu write cr_dpm_task_control @ 0xbee0
CCu write cr_mte_otpm_csm_base @ 0xbf08
CCu write cr_mte_otpm_op @ 0xbf10
CCu read EUR_CR_EVENT_STATUS @ 0xbf20
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0xbf50
CCu read EUR_CR_BIF_BANK0 @ 0xc028
CCu read EUR_CR_BIF_CTRL @ 0xc0d0
CCu write EUR_CR_BIF_CTRL @ 0xc0f0
CCu write EUR_CR_BIF_CTRL @ 0xc0f8
CCu write EUR_CR_PDS_INV0 @ 0xc168
CCu write EUR_CR_PDS_INV_CSC @ 0xc170
CCu read cr_cache_ctrl @ 0xc178
CCu write cr_cache_ctrl @ 0xc190
CCu write cr_use_cache @ 0xc198
CCu read cr_pds_cache_status @ 0xc1b0
CCu write cr_pds_cache_host_clear @ 0xc1d8
CCu read EUR_CR_EVENT_STATUS @ 0xc1e8
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0xc210
CCu read EUR_CR_BIF_BANK0 @ 0xc260
CCu write EUR_CR_BIF_BANK0 @ 0xc2a0
CCu read EUR_CR_BIF_BANK0 @ 0xc2a8
CCu read EUR_CR_BIF_BANK0 @ 0xc320
CCu write EUR_CR_BIF_BANK0 @ 0xc388
CCu read EUR_CR_BIF_BANK0 @ 0xc390
CCu read EUR_CR_DPM_STATE_CONTEXT_ID @ 0xc420
CCu read EUR_CR_DPM_STATE_CONTEXT_ID @ 0xc560
CCu read EUR_CR_DPM_STATE_CONTEXT_ID @ 0xc5d0
CCu write EUR_CR_DPM_STATE_CONTEXT_ID @ 0xc5f0
CCu read EUR_CR_DPM_STATE_CONTEXT_ID @ 0xc6e8
CCu write EUR_CR_DPM_STATE_CONTEXT_ID @ 0xc708
CCu read EUR_CR_BIF_BANK_SET @ 0xc710
CCu write EUR_CR_BIF_BANK_SET @ 0xc728
CCu write cr_dpm_state_table_context_0_base @ 0xc750
CCu write cr_dpm_state_table_context_1_base @ 0xc760
CCu write cr_dpm_free_context @ 0xc920
CCu write cr_te_tpc_control @ 0xc930
CCu read EUR_CR_EVENT_STATUS @ 0xc940
CCu write EUR_CR_EVENT_HOST_CLEAR @ 0xc970
CCu read EUR_CR_TIMER @ 0xcc98
