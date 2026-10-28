.586p
; GLSMP.VXD: dynamically loaded Windows 9x VxD that runs Glidulator's render
; workers on the other CPU cores. Windows 9x uses one core only; this VxD
; starts the others (APs) by INIT/SIPI and calls functions of the calling
; process on them in ring 0, in that process's address space (its page
; directory entries are copied, the memory the workers touch is locked with
; _LinPageLock). glide2x.dll loads it with CreateFile("\\.\GLSMP.VXD") and
; unloads it by closing the handle; nothing is installed. Protocol: the
; DIOC_* codes below; caller: platform/win98/platform_win32.c. Stopped APs
; park in a hlt loop of the BIOS ROM (as platform/dos_ovl/smp_dos.c does).
; DDB/segments and the VMM constants follow the Windows 98 DDK VMM.INC (facts
; only); hand-written for Open Watcom wasm (no _FLAT keyword; _LGROUP group).

DDK_VERSION             equ 040Ah
UNDEFINED_INIT_ORDER    equ 080000000h
SYS_DYNAMIC_DEVICE_INIT equ 1Bh
SYS_DYNAMIC_DEVICE_EXIT equ 1Ch
W32_DEVICEIOCONTROL     equ 23h
ERROR_BUSY              equ 170         ; Win32 error code for a refused call

; DIOCPARAMS fields (VWIN32)
DP_CODE                 equ 0Ch         ; dwIoControlCode
DP_IN                   equ 10h         ; lpvInBuffer
DP_CBIN                 equ 14h         ; cbInBuffer
DP_OUT                  equ 18h         ; lpvOutBuffer
DP_CBOUT                equ 1Ch         ; cbOutBuffer
DP_CBRET                equ 20h         ; lpcbBytesReturned

; VMM services (VMM.INC: Dyna_Link_Int=20h, VMM_DEVICE_ID=1; ordinals verified
; against the WME DDK VMM.INC service table).
DYNA_LINK_INT           equ 20h
SVC_PageAllocate        equ 00010053h   ; ordinal 0x53
SVC_PageFree            equ 00010055h   ; ordinal 0x55
SVC_MapPhysToLinear     equ 0001006Ch   ; ordinal 0x6C
SVC_Get_System_Time     equ 0001003Fh   ; ordinal 0x3F: eax = ms since start,
                                        ; 1-ms accurate, async, uses EAX+flags
SVC_LinPageLock         equ 00010063h   ; ordinal 0x63 (page num, count, flags)
SVC_LinPageUnLock       equ 00010064h   ; ordinal 0x64

; _PageAllocate flags and page type (VMM.INC)
PAGEZEROINIT            equ 00000001h
PAGEUSEALIGN            equ 00000002h
PAGECONTIG              equ 00000004h
PAGEFIXED               equ 00000008h
PAGEMAPGLOBAL           equ 40000000h
PAGEMARKDIRTY           equ 80000000h
PG_SYS                  equ 1

; Block (physically contiguous, below 4 GB): 0000 AP page directory, 1000 its
; 0-4 MB identity page table, 2000 the APs' IDT. Only these need physical
; addresses (CR3, the PDE). The AP stacks are a second, fixed but not
; contiguous allocation: n*10000 is the stack of AP n (64 KB each, grows down
; from the next slot). A large contiguous block can be unavailable once
; memory is fragmented (seen on a real machine below 16 MB).
ALLOC_PAGES             equ 3
STACK_PAGES             equ 128         ; MAX_AP * STACK_SIZE / 4 KB
BLK_IDT                 equ 2000h
BLK_STACKS              equ 0           ; offset of AP 0's slot in the stacks
STACK_SIZE              equ 10000h      ; = 1 shl STACK_SHIFT
STACK_SHIFT             equ 16
MAX_AP                  equ 8           ; APs beyond this park unused
EXC_VECTORS             equ 32          ; IDT entries (exceptions 0-31 only;
                                        ; the APs never enable interrupts)
EXC_STUB_SIZE           equ 7           ; push imm8 (2) + jmp rel32 (5)
MAX_LOCKS               equ 128         ; ranges DIOC_LOCK can hold

; Job slot states (one slot per AP, see ap_pm32)
JOB_IDLE                equ 0
JOB_POSTED              equ 1           ; set by DIOC_RUN, taken by the AP
JOB_RUNNING             equ 2
JOB_DONE                equ 3           ; fn returned, result in g_job_ret
JOB_FAULT               equ 4           ; CPU exception, AP parked for good

; Local APIC (xAPIC MMIO) and IPI command bits, as in platform/dos_ovl/smp_dos.c
MSR_APICBASE            equ 1Bh
APIC_BSP                equ 100h        ; MSR 1Bh bit 8: this core is the BSP
APIC_X2                 equ 400h        ; MSR 1Bh bit 10: x2APIC mode (no MMIO)
APIC_EN                 equ 800h        ; MSR 1Bh bit 11: global enable
APIC_SVR                equ 0F0h
APIC_LVT0               equ 350h
APIC_LVT1               equ 360h
ICR_LO                  equ 300h
ICR_HI                  equ 310h
ICR_PENDING             equ 1000h
ICR_ALL_BUT_SELF        equ 0C0000h
ICR_ASSERT              equ 4000h
ICR_INIT                equ 500h
ICR_SIPI                equ 600h

; Trampoline page parameter offsets (must match aptramp.S).
P_GDTR                  equ 80h
P_IDTR                  equ 88h
P_CR4                   equ 90h
P_CR3                   equ 94h
P_CR0                   equ 98h
P_JMP                   equ 9Ch         ; 4 bytes offset + 2 bytes selector
P_DSEL                  equ 0A2h
P_ALIVE                 equ 0A4h        ; APs that reached the real-mode trampoline

; Waits in milliseconds (delay_ms, Get_System_Time). Intel MP init: INIT,
; 10 ms, SIPI, >= 200 us, SIPI.
INIT_WAIT_MS            equ 10
SIPI_WAIT_MS            equ 1
AP_RUN_MS               equ 20          ; after the SIPIs: let the APs report
PARK_WAIT_MS            equ 10          ; STOP: max. wait for the APs to park
ROM_PARK_WAIT_MS        equ 10          ; STOP: max. wait for the ROM hlt loop
ROM_PARK_COUNT          equ 10h         ; trampoline byte the parking APs count in
TIME_POLL_CAP           equ 1000000     ; Get_System_Time calls before a wait
                                        ; gives up (time not advancing)
; AP heartbeat pacing in pause-loop iterations -- no time, the APs cannot
; call VMM services. The stop flag is checked on every iteration.
AP_SPIN                 equ 00100000h

; DeviceIoControl codes (our own protocol)
DIOC_ECHO               equ 1           ; write CAFEBABE (smoke test)
DIOC_ALLOC              equ 2           ; allocate, return {handle,linear,phys,pages}
DIOC_FREE               equ 3           ; free
DIOC_SMP_START          equ 4           ; INIT/SIPI the APs, run ap_pm32 on them
DIOC_SMP_STATUS         equ 5           ; {alive, running, heartbeat, parked, MSR 1Bh}
DIOC_SMP_STOP           equ 6           ; INIT the APs back to wait-for-SIPI, free
DIOC_DELAY_TEST         equ 7           ; input {ms}: run delay_ms (timing check)
DIOC_LOCK               equ 8           ; input {linear, bytes}: _LinPageLock
DIOC_UNLOCK             equ 9           ; input {linear, bytes}: unlock that
                                        ; range; no input: every range
DIOC_RUN                equ 10          ; input {ap, fn, arg, fpu cw, mxcsr}
DIOC_JOB                equ 11          ; input {ap}: {state, ret, vector, eip, cr2}
DIOC_CLOSEHANDLE        equ -1          ; VWIN32: the handle was closed, also
                                        ; when Windows ends a killed process

; ---- segments: locked, class 'LCODE', one flat group (as VMM.INC) ----
_LTEXT segment dword public use32 'LCODE'
_LTEXT ends
_LDATA segment dword public use32 'LCODE'
_LDATA ends
_LGROUP group _LTEXT, _LDATA

_LDATA segment
; VxD_Desc_Block (full 0x50-byte layout from VMM.INC)
public GLSMP_DDB
GLSMP_DDB label dword
    dd 0                      ; DDB_Next
    dw DDK_VERSION            ; DDB_SDK_Version
    dw 0                      ; DDB_Req_Device_Number (UNDEFINED_DEVICE_ID)
    db DDK_VERSION shr 8      ; DDB_Dev_Major_Version
    db DDK_VERSION and 0FFh   ; DDB_Dev_Minor_Version
    dw 0                      ; DDB_Flags
    db 'GLSMP   '             ; DDB_Name (8)
    dd UNDEFINED_INIT_ORDER   ; DDB_Init_Order
    dd offset GLSMP_Control   ; DDB_Control_Proc
    dd 0,0,0,0                ; V86/PM API proc + CSIP
    dd 0                      ; DDB_Reference_Data
    dd 0                      ; DDB_Service_Table_Ptr
    dd 0                      ; DDB_Service_Table_Size
    dd 0                      ; DDB_Win32_Service_Table
    dd 'verP'                 ; DDB_Prev  ('Prev')
    dd 50h                    ; DDB_Size  (0x50)
    dd '1vsR'                 ; DDB_Reserved1 ('Rsv1')
    dd '2vsR'                 ; DDB_Reserved2 ('Rsv2')
    dd '3vsR'                 ; DDB_Reserved3 ('Rsv3')

; state: block (AP page directory, identity page table, IDT) and AP stacks
g_handle  dd 0               ; _PageAllocate handle (0 = none)
g_linear  dd 0               ; ring-0 linear address of the block
g_phys    dd 0               ; physical address (PAGECONTIG)
g_stk_handle dd 0            ; AP stacks: _PageAllocate handle
g_stk_linear dd 0            ;   and ring-0 linear address
; state: AP bring-up
g_tr_handle   dd 0           ; trampoline page (< 1 MB) _PageAllocate handle
g_tr_linear   dd 0           ; its ring-0 linear address
g_tr_phys     dd 0           ; its physical address (< 0xA0000, the SIPI vector)
g_apic_lin    dd 0           ; local APIC MMIO linear (0 = not mapped yet)
g_apic_msr_lo dd 0           ; saved MSR 1Bh (restored after each IPI burst)
g_apic_msr_hi dd 0
g_win_pd_lin  dd 0           ; Windows' live page directory, mapped to read PDEs
g_running     dd 0           ; 1 while the APs are up
g_smp_status  dd 0           ; last START result (0 = ok, else failed step)
g_test_xor_lo dd 0           ; test hook: XORed into the MSR 1Bh value that
g_test_xor_hi dd 0           ;   check_apic sees (START input, 0 normally)
g_ap_running  dd 0           ; APs that reached ap_pm32 (protected mode, paging)
g_ap_hb       dd 0           ; heartbeat: the APs keep incrementing this
g_ap_stop     dd 0           ; set to 1 to make the APs leave their loop (cli/hlt)
g_ap_parked   dd 0           ; APs that saw g_ap_stop and halted
gdtr_buf      db 6 dup(0)    ; sgdt/sidt scratch
idtr_buf      db 6 dup(0)
g_busy        dd 0           ; bit 0: a state-changing DIOC call is running
; state: topology (one worker per physical core, as platform/dos_ovl/smp_dos.c)
g_max_leaf    dd 0           ; CPUID max standard leaf (BSP; the APs reuse it)
g_smt_shift   dd 0           ; APIC ID bits of the hyperthreads in a core
g_bsp_core    dd 0           ; BSP's APIC ID >> g_smt_shift (and 255)
g_core_claim  dd 8 dup(0)    ; bit per core: an AP has taken it
g_ap_sibling  dd 0           ; APs that parked as a second thread of a core
g_ap_apic     dd MAX_AP dup(0)   ; APIC ID of AP index n
; state: parking in the BIOS ROM after STOP
g_rom_hlt     dd 1           ; physical address of "hlt; jmp $-1" (F4 EB FD) in
                             ;   the BIOS ROM, 0 = none, 1 = not searched yet
g_rom_parked  dd 0           ; APs that reached the ROM hlt loop at the last STOP
; state: jobs on the APs (index = order in which the APs reached ap_pm32)
g_ap_next     dd 0           ; next AP index to hand out
g_ap_faults   dd 0           ; APs that took a CPU exception
g_job_state   dd MAX_AP dup(0)
g_job_fn      dd MAX_AP dup(0)   ; cdecl int fn(void *arg), in the caller's process
g_job_arg     dd MAX_AP dup(0)
g_job_fpucw   dd MAX_AP dup(0)   ; caller's x87 control word and MXCSR, loaded
g_job_mxcsr   dd MAX_AP dup(0)   ;   before fn (bit-identical results)
g_job_ret     dd MAX_AP dup(0)
g_fault_vec   dd MAX_AP dup(0)
g_fault_eip   dd MAX_AP dup(0)
g_fault_cr2   dd MAX_AP dup(0)
; state: locked ranges of the caller's process {first page, pages}
g_lock_n      dd 0
g_lock_tab    dd MAX_LOCKS*2 dup(0)
g_lock_ctx    dd 0           ; live PDE of the first locked range at lock time
g_lock_ctx_i  dd 0           ;   and its index: identify the locking process
g_locks_left  dd 0           ; pages left locked by a close in another context
g_win_pd_phys dd 0           ; physical address g_win_pd_lin maps
g_pd_added    dd 0           ; PDEs LOCK added to the AP page directory
g_pd_clash    dd 0           ; PDEs that differed from a present AP PDE (kept)
g_pd_resync   dd 0           ; present PDEs RUN replaced (idle resync)

; AP startup trampoline bytes (aptramp.S -> aptramp.bin -> aptramp.inc)
include aptramp.inc
_LDATA ends

_LTEXT segment
    assume cs:_LGROUP, ds:_LGROUP, es:_LGROUP, ss:_LGROUP
; Control proc: message in EAX; carry clear = success. Preserves ebx/esi/edi/ebp.
GLSMP_Control proc
    cmp eax, SYS_DYNAMIC_DEVICE_INIT
    je  short ctl_ok
    cmp eax, SYS_DYNAMIC_DEVICE_EXIT
    je  short ctl_exit
    cmp eax, W32_DEVICEIOCONTROL
    je  short ctl_dioc
    clc
    ret
ctl_ok:
    clc
    ret
ctl_exit:
    ; Not locked: the exit comes only after the last handle is closed, so no
    ; DeviceIoControl call can still be running.
    call do_free            ; stops the APs, then frees the block and stacks if held
    clc
    ret
ctl_dioc:
    call do_dioc            ; eax = 0 (success) or a Win32 error code
    clc
    ret
GLSMP_Control endp

; ---- DeviceIoControl handler (esi -> DIOCPARAMS). EBP keeps the DIOCPARAMS
;      pointer for the whole call (per call, no globals: a second thread's call
;      cannot overwrite it); no routine here and no VMM service changes EBP.
;      Calls that touch the shared state (block, trampoline page, APs) run one
;      at a time under g_busy; a second one meanwhile fails with ERROR_BUSY
;      instead of waiting. ECHO and DELAY touch no state and are not locked.
;      Returns eax = 0 or the error code. ----
do_dioc proc
    push ebx
    push esi
    push edi
    push ebp
    mov ebp, esi
    mov eax, [ebp+DP_CODE]

    cmp eax, DIOC_ECHO
    je  dio_echo
    cmp eax, DIOC_DELAY_TEST
    je  dio_delay_test

    lock bts dword ptr [g_busy], 0
    jnc short dio_locked
    mov eax, ERROR_BUSY
    jmp dio_out
dio_locked:
    cmp eax, DIOC_ALLOC
    je  dio_alloc
    cmp eax, DIOC_FREE
    je  dio_free
    cmp eax, DIOC_SMP_START
    je  dio_smp_start
    cmp eax, DIOC_SMP_STATUS
    je  dio_smp_status
    cmp eax, DIOC_SMP_STOP
    je  dio_smp_stop
    cmp eax, DIOC_LOCK
    je  dio_lock
    cmp eax, DIOC_UNLOCK
    je  dio_unlock_pages
    cmp eax, DIOC_RUN
    je  dio_run
    cmp eax, DIOC_JOB
    je  dio_job
    cmp eax, DIOC_CLOSEHANDLE
    je  dio_close
    jmp dio_unlock

dio_delay_test:
    cmp dword ptr [ebp+DP_CBIN], 4
    jb  dio_ret
    mov ecx, [ebp+DP_IN]               ; {ms}
    test ecx, ecx
    jz  dio_ret
    mov ecx, [ecx]
    call delay_ms
    jmp dio_ret

dio_echo:
    mov edi, [ebp+DP_OUT]
    test edi, edi
    jz  dio_ret
    cmp dword ptr [ebp+DP_CBOUT], 4
    jb  dio_ret
    mov dword ptr [edi], 0CAFEBABEh
    mov eax, 4
    call set_cbret
    jmp dio_ret

dio_alloc:
    call ensure_block
    jc   dio_unlock
    mov edi, [ebp+DP_OUT]
    test edi, edi
    jz  dio_unlock
    cmp dword ptr [ebp+DP_CBOUT], 16
    jb  dio_unlock
    mov eax, [g_handle]
    mov [edi], eax
    mov eax, [g_linear]
    mov [edi+4], eax
    mov eax, [g_phys]
    mov [edi+8], eax
    mov dword ptr [edi+12], ALLOC_PAGES
    mov eax, 16
    call set_cbret
    jmp dio_unlock

dio_free:
    call do_free
    jmp dio_unlock

dio_smp_start:
    mov dword ptr [g_test_xor_lo], 0   ; optional input {xor lo, xor hi}
    mov dword ptr [g_test_xor_hi], 0
    cmp dword ptr [ebp+DP_CBIN], 8
    jb  short dss_noin
    mov ecx, [ebp+DP_IN]
    test ecx, ecx
    jz  short dss_noin
    mov eax, [ecx]
    mov [g_test_xor_lo], eax
    mov eax, [ecx+4]
    mov [g_test_xor_hi], eax
dss_noin:
    call do_smp_start
    mov edi, [ebp+DP_OUT]
    test edi, edi
    jz  dio_unlock
    cmp dword ptr [ebp+DP_CBOUT], 24
    jb  dio_unlock
    call ap_alive_eax          ; eax = P_ALIVE (0 if no trampoline page)
    mov [edi], eax
    mov eax, [g_ap_running]
    mov [edi+4], eax
    mov eax, [g_ap_hb]
    mov [edi+8], eax
    mov eax, [g_tr_phys]
    mov [edi+12], eax
    mov eax, [g_apic_lin]
    mov [edi+16], eax
    mov eax, [g_smp_status]
    mov [edi+20], eax
    mov eax, 24
    cmp dword ptr [ebp+DP_CBOUT], 40
    jb  short dss_short
    mov ecx, [g_ap_next]           ; usable APs (worker slots, one per core)
    mov [edi+24], ecx
    mov ecx, [g_ap_sibling]
    mov [edi+28], ecx
    mov ecx, [g_smt_shift]
    mov [edi+32], ecx
    mov ecx, [g_bsp_core]
    mov [edi+36], ecx
    mov eax, 40
dss_short:
    call set_cbret
    jmp dio_unlock

dio_smp_status:
    mov edi, [ebp+DP_OUT]
    test edi, edi
    jz  dio_unlock
    cmp dword ptr [ebp+DP_CBOUT], 12
    jb  dio_unlock
    call ap_alive_eax
    mov [edi], eax
    mov eax, [g_ap_running]
    mov [edi+4], eax
    mov eax, [g_ap_hb]
    mov [edi+8], eax
    mov eax, 12
    cmp dword ptr [ebp+DP_CBOUT], 16
    jb  dst_short
    mov ecx, [g_ap_parked]
    mov [edi+12], ecx
    mov eax, 16
    cmp dword ptr [ebp+DP_CBOUT], 20
    jb  dst_short
    mov ecx, MSR_APICBASE          ; MSR 1Bh now (APIC must be off outside
    rdmsr                          ; ipi_once: bit 11 clear under Win98)
    mov [edi+16], eax
    mov eax, 20
    cmp dword ptr [ebp+DP_CBOUT], 24
    jb  dst_short
    mov ecx, [g_ap_faults]
    mov [edi+20], ecx
    mov eax, 24
    cmp dword ptr [ebp+DP_CBOUT], 28
    jb  dst_short
    mov ecx, [g_locks_left]
    mov [edi+24], ecx
    mov eax, 28
    cmp dword ptr [ebp+DP_CBOUT], 40
    jb  dst_short
    mov ecx, [g_pd_added]
    mov [edi+28], ecx
    mov ecx, [g_pd_clash]
    mov [edi+32], ecx
    mov ecx, [g_lock_n]
    mov [edi+36], ecx
    mov eax, 40
    cmp dword ptr [ebp+DP_CBOUT], 48
    jb  dst_short
    mov ecx, [g_rom_parked]
    mov [edi+40], ecx
    mov ecx, [g_rom_hlt]
    mov [edi+44], ecx
    mov eax, 48
    cmp dword ptr [ebp+DP_CBOUT], 52
    jb  dst_short
    mov ecx, [g_pd_resync]
    mov [edi+48], ecx
    mov eax, 52
dst_short:
    call set_cbret
    jmp dio_unlock

dio_lock:
    call do_lock               ; eax = status
    jmp  short dio_status_out
dio_unlock_pages:
    cmp  dword ptr [ebp+DP_CBIN], 8
    jb   short dup_all
    cmp  dword ptr [ebp+DP_IN], 0
    je   short dup_all
    call do_unlock_range       ; eax = status
    jmp  dio_status_out
dup_all:
    call do_unlock_all
    jmp  dio_unlock
dio_run:
    call do_run                ; eax = status
dio_status_out:
    mov  edi, [ebp+DP_OUT]
    test edi, edi
    jz   dio_unlock
    cmp  dword ptr [ebp+DP_CBOUT], 4
    jb   dio_unlock
    mov  [edi], eax
    mov  eax, 4
    call set_cbret
    jmp  dio_unlock

dio_job:
    cmp  dword ptr [ebp+DP_CBIN], 4
    jb   dio_unlock
    mov  ecx, [ebp+DP_IN]
    test ecx, ecx
    jz   dio_unlock
    mov  ecx, [ecx]            ; ap
    cmp  ecx, MAX_AP
    jae  dio_unlock
    mov  edi, [ebp+DP_OUT]
    test edi, edi
    jz   dio_unlock
    cmp  dword ptr [ebp+DP_CBOUT], 20
    jb   dio_unlock
    mov  eax, [g_job_state+ecx*4]
    mov  [edi], eax
    mov  eax, [g_job_ret+ecx*4]
    mov  [edi+4], eax
    mov  eax, [g_fault_vec+ecx*4]
    mov  [edi+8], eax
    mov  eax, [g_fault_eip+ecx*4]
    mov  [edi+12], eax
    mov  eax, [g_fault_cr2+ecx*4]
    mov  [edi+16], eax
    mov  eax, 20
    call set_cbret
    jmp  dio_unlock

dio_close:
    call do_close
    jmp  dio_unlock

dio_smp_stop:
    call do_smp_stop
dio_unlock:
    mov dword ptr [g_busy], 0
dio_ret:
    xor eax, eax
dio_out:
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret
do_dioc endp

; eax <- P_ALIVE from the trampoline page (0 if none allocated)
ap_alive_eax proc
    mov edx, [g_tr_linear]
    xor eax, eax
    test edx, edx
    jz  short aae_done
    mov eax, [edx+P_ALIVE]
aae_done:
    ret
ap_alive_eax endp

; write EAX into *lpcbBytesReturned if present (ebp -> DIOCPARAMS)
set_cbret proc
    mov ecx, [ebp+DP_CBRET]
    test ecx, ecx
    jz  short cbret_done
    mov [ecx], eax
cbret_done:
    ret
set_cbret endp

; ---- block and AP stacks: allocate once, reuse while held. CF on failure. ----
ensure_block proc
    cmp dword ptr [g_handle], 0
    jne short eb_ok
    ; _PageAllocate(nPages, PG_SYS, 0, AlignMask, minPhys, maxPhys, &g_phys, flags)
    push PAGEUSEALIGN or PAGECONTIG or PAGEFIXED or PAGEZEROINIT
    push offset g_phys
    push 100000h           ; maxPhys page number: below 4 GB (CR3 is 32 bits;
                           ;   0FFFFFFFFh would mean below 16 MB)
    push 0                 ; minPhys
    push 0                 ; AlignMask (page alignment only)
    push 0                 ; hVM (ignored for PG_SYS)
    push PG_SYS
    push ALLOC_PAGES
    int  DYNA_LINK_INT
    dd   SVC_PageAllocate
    add  esp, 32
    test eax, eax
    jz   short eb_fail
    mov  [g_handle], eax
    mov  [g_linear], edx
eb_ok:
    cmp  dword ptr [g_stk_handle], 0
    jne  short eb_done
    push PAGEFIXED         ; AP stacks: fixed, any physical pages
    push 0
    push 0
    push 0
    push 0
    push 0
    push PG_SYS
    push STACK_PAGES
    int  DYNA_LINK_INT
    dd   SVC_PageAllocate
    add  esp, 32
    test eax, eax
    jz   short eb_fail
    mov  [g_stk_handle], eax
    mov  [g_stk_linear], edx
eb_done:
    clc
    ret
eb_fail:
    stc
    ret
ensure_block endp

; free the block and the stacks if held (_PageFree). They hold the APs'
; page directory and page table, so the APs are stopped (INIT) first.
do_free proc
    call do_smp_stop
    mov eax, [g_handle]
    test eax, eax
    jz  short free_done
    push 0
    push eax
    int  DYNA_LINK_INT
    dd   SVC_PageFree
    add  esp, 8
    mov  dword ptr [g_handle], 0
    mov  dword ptr [g_linear], 0
    mov  dword ptr [g_phys], 0
free_done:
    mov  eax, [g_stk_handle]
    test eax, eax
    jz   short free_stk_done
    push 0
    push eax
    int  DYNA_LINK_INT
    dd   SVC_PageFree
    add  esp, 8
    mov  dword ptr [g_stk_handle], 0
    mov  dword ptr [g_stk_linear], 0
free_stk_done:
    ret
do_free endp

; ---- map the local APIC MMIO page once (g_apic_lin). CF set on failure. ----
map_apic proc
    cmp dword ptr [g_apic_lin], 0
    jne short ma_done
    mov ecx, MSR_APICBASE
    rdmsr
    mov [g_apic_msr_lo], eax
    mov [g_apic_msr_hi], edx
    mov ebx, eax
    and ebx, 0FFFFF000h
    jnz short ma_have
    mov ebx, 0FEE00000h        ; reset default if the base reads 0
ma_have:
    push 0                     ; _MapPhysToLinear(phys, nBytes, flags) cdecl
    push 1000h
    push ebx
    int  DYNA_LINK_INT
    dd   SVC_MapPhysToLinear
    add  esp, 12
    cmp  eax, 0FFFFFFFFh
    je   short ma_fail
    mov  [g_apic_lin], eax
ma_done:
    clc
    ret
ma_fail:
    stc
    ret
map_apic endp

; ---- allocate the trampoline page below 1 MB (SIPI vector). CF on failure. ----
alloc_tramp proc
    cmp dword ptr [g_tr_handle], 0
    jne short at_ok
    push PAGEUSEALIGN or PAGECONTIG or PAGEFIXED or PAGEZEROINIT
    push offset g_tr_phys
    push 09Fh              ; maxPhys page number -> phys < 0xA0000 (below video)
    push 010h              ; minPhys page number -> phys >= 0x10000 (above IVT)
    push 0                 ; AlignMask (page alignment)
    push 0                 ; hVM
    push PG_SYS
    push 1                 ; one page
    int  DYNA_LINK_INT
    dd   SVC_PageAllocate
    add  esp, 32
    test eax, eax
    jz   short at_fail
    mov  [g_tr_handle], eax
    mov  [g_tr_linear], edx
    mov  eax, [g_tr_phys]
    test eax, 0FFFh        ; must be page-aligned ...
    jnz  short at_bad
    cmp  eax, 0A0000h      ; ... and below 1 MB (8-bit SIPI vector)
    jae  short at_bad
at_ok:
    clc
    ret
at_bad:
    push 0                 ; unusable page -> free it
    push dword ptr [g_tr_handle]
    int  DYNA_LINK_INT
    dd   SVC_PageFree
    add  esp, 8
    mov  dword ptr [g_tr_handle], 0
    mov  dword ptr [g_tr_linear], 0
at_fail:
    stc
    ret
alloc_tramp endp

; ---- build the AP page directory in the block: all PDEs copied from the
;      live page directory, PDE[0] replaced by an own identity page table for
;      0-4 MB (trampoline page and the instruction after "mov cr0" stay
;      mapped; Win32 code and data never live below 4 MB). START runs in the
;      caller's context, so PDEs 1-511 are that process's private arena (the
;      jobs' code and data; its page tables stay valid while another process
;      runs, only the PDEs are swapped), 512-1023 the shared/system arena
;      (VxD code+data, APIC, block). Memory the caller maps after START is
;      missing here if it needs a new page table. CF set on failure. ----
build_pd proc
    call map_win_pd
    jc   short bp_fail
    push es
    mov  ax, ds
    mov  es, ax
    mov  edi, [g_linear]
    mov  esi, [g_win_pd_lin]
    mov  ecx, 1024
    rep  movsd
    pop  es
    mov  edi, [g_linear]       ; PDE[0] -> our identity page table (g_phys+0x1000)
    mov  eax, [g_phys]
    add  eax, 1000h
    or   eax, 3                ; present, read/write
    mov  [edi], eax
    mov  edi, [g_linear]       ; identity PT: linear 0-4 MB == physical 0-4 MB
    add  edi, 1000h
    mov  eax, 3
    mov  ecx, 1024
bp_pt:
    mov  [edi], eax
    add  edi, 4
    add  eax, 1000h
    dec  ecx
    jnz  short bp_pt
    clc
    ret
bp_fail:
    stc
    ret
build_pd endp

; ---- map the live page directory (CR3) once; remapped only if CR3 moves
;      (the classic VMM never frees _MapPhysToLinear mappings). Returns
;      eax = g_win_pd_lin, CF set on failure. ----
map_win_pd proc
    mov  eax, cr3
    and  eax, 0FFFFF000h
    cmp  dword ptr [g_win_pd_lin], 0
    je   short mwp_map
    cmp  eax, [g_win_pd_phys]
    je   short mwp_done
mwp_map:
    push eax
    push 0
    push 1000h
    push eax
    int  DYNA_LINK_INT
    dd   SVC_MapPhysToLinear
    add  esp, 12
    pop  ecx
    cmp  eax, 0FFFFFFFFh
    je   short mwp_fail
    mov  [g_win_pd_lin], eax
    mov  [g_win_pd_phys], ecx
mwp_done:
    mov  eax, [g_win_pd_lin]
    clc
    ret
mwp_fail:
    stc
    ret
map_win_pd endp

; eax <- live PDE g_lock_ctx_i (0 if unmapped)
lock_ctx_pde proc
    call map_win_pd
    jc   short lcp_none
    mov  ecx, [g_lock_ctx_i]
    mov  eax, [eax+ecx*4]
    and  eax, 0FFFFF001h       ; frame + present (A/D bits change by use)
    ret
lcp_none:
    xor  eax, eax
    ret
lock_ctx_pde endp

; ---- DIOC_CLOSEHANDLE: the client is gone or done -- also sent when Windows
;      ends a killed process, before it frees the process memory the jobs
;      run on. Stop the APs, unlock (only in the locking process's context:
;      page numbers are per process; elsewhere the locks are left and
;      counted), free the block. ----
do_close proc
    call do_smp_stop
    cmp  dword ptr [g_lock_n], 0
    je   short dc_free
    call lock_ctx_pde
    cmp  eax, [g_lock_ctx]
    jne  short dc_foreign
    call do_unlock_all
    jmp  short dc_free
dc_foreign:
    mov  ecx, [g_lock_n]
dc_count:
    mov  eax, [g_lock_tab+ecx*8-8+4]
    add  [g_locks_left], eax
    dec  ecx
    jnz  short dc_count
    mov  dword ptr [g_lock_n], 0
dc_free:
    call do_free
    ret
do_close endp

; ---- copy the trampoline into its page and fill in its parameters ----
fill_tramp proc
    push es
    mov  ax, ds
    mov  es, ax
    mov  esi, offset AP_TRAMP
    mov  edi, [g_tr_linear]
    mov  ecx, AP_TRAMP_LEN
    rep  movsb
    pop  es
    mov  edi, [g_tr_linear]
    sgdt gdtr_buf              ; P_GDTR: 2 bytes limit + 4 bytes base
    mov  eax, dword ptr gdtr_buf
    mov  [edi+P_GDTR], eax
    mov  ax, word ptr gdtr_buf+4
    mov  [edi+P_GDTR+4], ax
    mov  word ptr [edi+P_IDTR], EXC_VECTORS*8-1   ; P_IDTR: our IDT (build_idt)
    mov  eax, [g_linear]
    add  eax, BLK_IDT
    mov  [edi+P_IDTR+2], eax
    mov  eax, cr4
    mov  [edi+P_CR4], eax
    mov  eax, [g_phys]         ; P_CR3 = our AP page directory
    mov  [edi+P_CR3], eax
    mov  eax, cr0
    mov  [edi+P_CR0], eax
    mov  eax, offset ap_pm32   ; P_JMP: offset + selector (VxD ring-0 code = CS)
    mov  [edi+P_JMP], eax
    mov  ax, cs
    mov  [edi+P_JMP+4], ax
    mov  ax, ds                ; P_DSEL: DS/ES/SS for ap_pm32 (flat ring-0 data)
    mov  [edi+P_DSEL], ax
    mov  dword ptr [edi+P_ALIVE], 0
    ret
fill_tramp endp

; ---- send one IPI with the local APIC switched on only for that moment
;      (eax = ICR low command; preserves ebx/esi/edi). Interrupts stay off while the APIC is on: an
;      interrupt taken through the APIC could arrive as its spurious vector
;      (SVR 0xFF), which has no handler in Windows' IDT. The waits between
;      the IPIs run with the APIC off again, as Windows 98 expects it. ----
ipi_once proc
    push ebx
    push esi
    mov  ebx, eax
    pushfd
    cli
    call apic_enable
    mov  eax, ebx
    call send_ipi
    call apic_restore
    popfd
    pop  esi
    pop  ebx
    ret
ipi_once endp

; ---- enable the local APIC on this (boot) core (only inside ipi_once) ----
apic_enable proc
    mov ecx, MSR_APICBASE
    rdmsr
    mov [g_apic_msr_lo], eax
    mov [g_apic_msr_hi], edx
    or  eax, APIC_EN           ; Windows 98 clears the global-enable bit
    wrmsr
    mov esi, [g_apic_lin]
    test dword ptr [esi+APIC_SVR], 100h
    jnz short ae_done
    mov dword ptr [esi+APIC_SVR], 1FFh   ; software-enable, spurious vector 0xFF
    mov dword ptr [esi+APIC_LVT0], 700h  ; LINT0 = ExtINT (8259)
    mov dword ptr [esi+APIC_LVT1], 400h  ; LINT1 = NMI
ae_done:
    ret
apic_enable endp

; restore MSR 1Bh to the value Windows left (local APIC off again)
apic_restore proc
    mov ecx, MSR_APICBASE
    mov eax, [g_apic_msr_lo]
    mov edx, [g_apic_msr_hi]
    wrmsr
    ret
apic_restore endp

; send one IPI (eax = ICR low command incl. destination shorthand), wait for
; delivery. All-but-self shorthand -> no destination field needed.
send_ipi proc
    mov esi, [g_apic_lin]
    mov dword ptr [esi+ICR_HI], 0
    mov dword ptr [esi+ICR_LO], eax      ; writing ICR low sends the IPI
    mov ecx, 100000
si_wait:
    test dword ptr [esi+ICR_LO], ICR_PENDING
    jz  short si_done
    dec ecx
    jnz short si_wait
si_done:
    ret
send_ipi endp

; wait at least ecx milliseconds (Get_System_Time, wrap-safe). The clock's
; phase is unknown, so wait until more than ecx ms have passed. Gives up after
; TIME_POLL_CAP calls if the time does not advance. Preserves ebx/esi.
delay_ms proc
    push ebx
    push esi
    lea  ebx, [ecx+1]
    int  DYNA_LINK_INT
    dd   SVC_Get_System_Time
    mov  esi, eax              ; start
    mov  ecx, TIME_POLL_CAP
dm_loop:
    int  DYNA_LINK_INT
    dd   SVC_Get_System_Time
    sub  eax, esi
    cmp  eax, ebx
    jae  short dm_done
    dec  ecx
    jnz  short dm_loop
dm_done:
    pop  esi
    pop  ebx
    ret
delay_ms endp

; STOP: wait until every AP that reached ap_pm32 has parked, at most
; PARK_WAIT_MS. Preserves ebx/esi.
wait_parked proc
    push ebx
    push esi
    int  DYNA_LINK_INT
    dd   SVC_Get_System_Time
    mov  esi, eax
    mov  ecx, TIME_POLL_CAP
wp_loop:
    mov  eax, [g_ap_parked]
    cmp  eax, [g_ap_running]
    jae  short wp_done
    int  DYNA_LINK_INT
    dd   SVC_Get_System_Time
    sub  eax, esi
    cmp  eax, PARK_WAIT_MS+1
    jae  short wp_done
    dec  ecx
    jnz  short wp_loop
wp_done:
    pop  esi
    pop  ebx
    ret
wait_parked endp

; ---- DIOC_SMP_START: build everything, INIT/SIPI the APs, let them run ----
do_smp_start proc
    cmp dword ptr [g_running], 0
    je  short ss_go            ; not up yet -> bring the APs up
    ret                        ; already up -> caller samples the counters
ss_go:
    call check_apic            ; before anything is allocated
    mov  [g_smp_status], eax
    test eax, eax
    jz   short ss_check_ok
    ret
ss_check_ok:
    call ensure_block
    jnc  short ss_block_ok
    mov  dword ptr [g_smp_status], 1
    ret
ss_block_ok:
    call map_apic
    jnc  short ss_apic_ok
    mov  dword ptr [g_smp_status], 2
    ret
ss_apic_ok:
    call alloc_tramp
    jnc  short ss_tramp_ok
    mov  dword ptr [g_smp_status], 3
    ret
ss_tramp_ok:
    call build_pd
    jnc  short ss_pd_ok
    mov  dword ptr [g_smp_status], 4
    ret
ss_pd_ok:
    call build_idt
    call fill_tramp
    call reset_jobs
    call cpu_topo
    mov  dword ptr [g_ap_running], 0
    mov  dword ptr [g_ap_hb], 0
    mov  dword ptr [g_ap_stop], 0
    mov  dword ptr [g_ap_parked], 0
    mov  dword ptr [g_running], 1
    ; INIT, 10 ms, two SIPIs with the trampoline-page vector
    mov  eax, ICR_ALL_BUT_SELF or ICR_ASSERT or ICR_INIT
    call ipi_once
    mov  ecx, INIT_WAIT_MS
    call delay_ms
    mov  eax, [g_tr_phys]
    shr  eax, 12               ; SIPI vector = physical page number
    or   eax, ICR_ALL_BUT_SELF or ICR_SIPI
    call ipi_once
    mov  ecx, SIPI_WAIT_MS
    call delay_ms
    mov  eax, [g_tr_phys]
    shr  eax, 12
    or   eax, ICR_ALL_BUT_SELF or ICR_SIPI
    call ipi_once
    mov  ecx, AP_RUN_MS        ; let the APs report and bump the heartbeat
    call delay_ms
    mov  eax, [g_tr_linear]    ; no AP reached the trampoline -> not a success
    cmp  dword ptr [eax+P_ALIVE], 0
    jne  short ss_done
    mov  dword ptr [g_smp_status], 9
ss_done:
    ret
do_smp_start endp

; ---- check the APIC as the DOS path does (smp_dos.c): the CPU has MSRs, the
;      APIC is in xAPIC mode (x2APIC ignores MMIO writes, START would report
;      success with no AP started), this core is the BSP, the base is below
;      4 GB. Returns eax = 0 or the START status code. ----
check_apic proc
    mov  eax, 1
    cpuid
    test edx, 20h              ; CPUID.1:EDX bit 5 = MSR (rdmsr would #GP)
    jnz  short ca_msr
    mov  eax, 5
    ret
ca_msr:
    mov  ecx, MSR_APICBASE
    rdmsr
    xor  eax, [g_test_xor_lo]
    xor  edx, [g_test_xor_hi]
    test eax, APIC_X2
    jz   short ca_xapic
    mov  eax, 6
    ret
ca_xapic:
    test eax, APIC_BSP
    jnz  short ca_bsp
    mov  eax, 7
    ret
ca_bsp:
    test edx, edx              ; base bits 32+ (MMIO not reachable)
    jz   short ca_ok
    mov  eax, 8
    ret
ca_ok:
    xor  eax, eax
    ret
check_apic endp

; ---- DIOC_SMP_STOP (and device exit): INIT the APs back to wait-for-SIPI ----
do_smp_stop proc
    cmp dword ptr [g_running], 0
    je  stp_done
    mov dword ptr [g_ap_stop], 1   ; ask the APs to leave the loop ...
    call wait_parked               ; ... and wait until they halted (max 10 ms)
    mov eax, ICR_ALL_BUT_SELF or ICR_ASSERT or ICR_INIT
    call ipi_once                  ; INIT: APs back to wait-for-SIPI ...
    mov ecx, 1
    call delay_ms
    push ebx
    push esi
    push edi
    call park_in_rom               ; ... and from there into the ROM hlt loop
    pop edi
    pop esi
    pop ebx
    mov dword ptr [g_running], 0
    mov eax, [g_tr_handle]         ; free the trampoline page
    test eax, eax
    jz  stp_done
    cmp dword ptr [g_rom_hlt], 0   ; parked in the ROM: only once every AP
    je  stp_free             ;   has left the page; otherwise keep it
    mov ecx, [g_tr_linear]         ;   (one page lost, never code freed
    mov edx, [ecx+P_ALIVE]         ;   under a running core)
    cmp [g_rom_parked], edx
    jae stp_free
    mov dword ptr [g_tr_handle], 0
    jmp stp_forget
stp_free:
    push 0
    push eax
    int  DYNA_LINK_INT
    dd   SVC_PageFree
    add  esp, 8
    mov  dword ptr [g_tr_handle], 0
stp_forget:
    mov  dword ptr [g_tr_linear], 0
    mov  dword ptr [g_tr_phys], 0
stp_done:
    ret
do_smp_stop endp

; ---- APs' IDT in the block: exceptions 0-31 -> exc_stub_n (32-bit
;      interrupt gates, VxD code selector). Windows' IDT would run Windows'
;      handlers on a core Windows does not know about. ----
build_idt proc
    mov  edi, [g_linear]
    add  edi, BLK_IDT
    mov  edx, offset exc_stubs
    xor  ecx, ecx
bi_loop:
    mov  eax, edx              ; low dword: selector << 16 | offset 15..0
    and  eax, 0FFFFh
    mov  ebx, cs
    shl  ebx, 16
    or   eax, ebx
    mov  [edi+ecx*8], eax
    mov  eax, edx              ; high dword: offset 31..16 | P, DPL 0, 32-bit int gate
    and  eax, 0FFFF0000h
    or   eax, 8E00h
    mov  [edi+ecx*8+4], eax
    add  edx, EXC_STUB_SIZE
    inc  ecx
    cmp  ecx, EXC_VECTORS
    jb   short bi_loop
    ret
build_idt endp

; clear the job slots and the AP numbering (START, before the SIPIs)
reset_jobs proc
    push es
    mov  ax, ds
    mov  es, ax
    mov  edi, offset g_job_state
    xor  eax, eax
    mov  ecx, MAX_AP
    rep  stosd
    pop  es
    mov  dword ptr [g_ap_next], 0
    mov  dword ptr [g_ap_faults], 0
    mov  dword ptr [g_pd_added], 0
    mov  dword ptr [g_pd_clash], 0
    mov  dword ptr [g_pd_resync], 0
    mov  dword ptr [g_ap_sibling], 0
    push es
    mov  ax, ds
    mov  es, ax
    mov  edi, offset g_core_claim
    xor  eax, eax
    mov  ecx, 8
    rep  stosd
    pop  es
    ret
reset_jobs endp

; ---- topology of the BSP (as smp_dos.c my_apic_id/smt_shift): sets
;      g_max_leaf, g_smt_shift, g_bsp_core. Hyperthreads of one core differ
;      only in the low g_smt_shift bits of the APIC ID. ----
cpu_topo proc
    push ebx
    xor  eax, eax
    cpuid
    mov  [g_max_leaf], eax
    mov  dword ptr [g_smt_shift], 0
    cmp  eax, 0Bh
    jb   short ct_leaf1
    mov  eax, 0Bh
    xor  ecx, ecx
    cpuid
    test ebx, 0FFFFh
    jz   short ct_leaf1
    shr  ecx, 8
    and  ecx, 0FFh
    cmp  ecx, 1                ; level 0 = SMT
    jne  short ct_leaf1
    and  eax, 31
    mov  [g_smt_shift], eax
    jmp  ct_id
ct_leaf1:
    mov  eax, 1
    cpuid
    test edx, 10000000h        ; HTT: more than one logical CPU per package
    jz   ct_id
    cmp  dword ptr [g_max_leaf], 4
    jb   short ct_limited
    shr  ebx, 16
    and  ebx, 0FFh             ; logical CPUs per package
    push ebx
    mov  eax, 4
    xor  ecx, ecx
    cpuid
    pop  ebx
    shr  eax, 26
    inc  eax                   ; cores per package
    xchg eax, ebx
    xor  edx, edx
    div  ebx                   ; eax = threads per core
    xor  ecx, ecx
ct_log2:
    mov  edx, 1
    shl  edx, cl
    cmp  edx, eax
    jae  short ct_shift
    inc  ecx
    jmp  short ct_log2
ct_shift:
    mov  [g_smt_shift], ecx
    jmp  short ct_id
ct_limited:                    ; "CPUID Limit" hides leaves 4 and 0Bh: as
    mov  edx, eax              ; smt_shift() in smp_dos.c, Intel family 6
    shr  edx, 8                ; model 1Ah+ (not 1Dh) keeps 1 SMT bit
    and  edx, 15
    cmp  edx, 6
    jne  short ct_id
    mov  edx, eax
    shr  edx, 4
    and  edx, 15
    shr  eax, 12
    and  eax, 0F0h
    or   eax, edx              ; model
    cmp  eax, 1Ah
    jb   short ct_id
    cmp  eax, 1Dh
    je   short ct_id
    xor  eax, eax
    cpuid
    cmp  ebx, 756E6547h        ; "GenuineIntel"
    jne  short ct_id
    cmp  edx, 49656E69h
    jne  short ct_id
    cmp  ecx, 6C65746Eh
    jne  short ct_id
    mov  dword ptr [g_smt_shift], 1
ct_id:
    call apic_id_eax
    mov  ecx, [g_smt_shift]
    shr  eax, cl
    and  eax, 0FFh
    mov  [g_bsp_core], eax
    pop  ebx
    ret
cpu_topo endp

; eax <- this core's APIC ID (CPUID 0Bh if present, else leaf 1). Uses
; ebx/ecx/edx, no stack (the APs call it through ap_apic_id below).
apic_id_eax proc
    cmp  dword ptr [g_max_leaf], 0Bh
    jb   short ai_leaf1
    mov  eax, 0Bh
    xor  ecx, ecx
    cpuid
    test ebx, 0FFFFh
    jz   short ai_leaf1
    mov  eax, edx
    ret
ai_leaf1:
    mov  eax, 1
    cpuid
    mov  eax, ebx
    shr  eax, 24
    ret
apic_id_eax endp

; ---- BIOS ROM: find "hlt; jmp $-1" (F4 EB FD) once, g_rom_hlt = physical
;      address or 0. The ROM is mapped once and never unmapped (the classic
;      VMM never frees _MapPhysToLinear mappings). ----
find_rom_hlt proc
    cmp  dword ptr [g_rom_hlt], 1
    jne  short frh_done
    mov  dword ptr [g_rom_hlt], 0
    push 0
    push 10000h
    push 0F0000h
    int  DYNA_LINK_INT
    dd   SVC_MapPhysToLinear
    add  esp, 12
    cmp  eax, 0FFFFFFFFh
    je   short frh_done
    xor  ecx, ecx
frh_loop:
    cmp  byte ptr [eax+ecx], 0F4h
    jne  short frh_next
    cmp  byte ptr [eax+ecx+1], 0EBh
    jne  short frh_next
    cmp  byte ptr [eax+ecx+2], 0FDh
    jne  short frh_next
    lea  eax, [ecx+0F0000h]
    mov  [g_rom_hlt], eax
    ret
frh_next:
    inc  ecx
    cmp  ecx, 10000h-2
    jb   short frh_loop
frh_done:
    ret
find_rom_hlt endp

; ---- after the INIT of STOP: one more SIPI into a few bytes in the
;      trampoline page that jump to the BIOS's hlt loop. A core in
;      wait-for-SIPI does not join the firmware's SMI rendezvous (every SMI
;      then waits for the BIOS timeout; on an i7-4770T under DOS that made
;      USB-legacy keyboard access 20x slower and MS-DOS ran out of stacks); a
;      halted core does, and nothing of ours stays in use. Each AP counts
;      itself in the page, which is freed only after all have left it. No
;      loop in the ROM: they stay in wait-for-SIPI. ----
park_in_rom proc
    mov  dword ptr [g_rom_parked], 0
    call find_rom_hlt
    mov  eax, [g_rom_hlt]
    test eax, eax
    jz   pr_done
    mov  edi, [g_tr_linear]
    mov  ebx, [edi+P_ALIVE]    ; APs that reached the trampoline = INITed
    test ebx, ebx
    jz   pr_done
    mov  dword ptr [edi], 0FEF02EFAh    ; cli; cs: lock inc byte [10h]: fa 2e f0 fe
    mov  dword ptr [edi+4], 0EA001006h  ; 06 10 00; jmp far: ea
    sub  eax, 0F0000h
    mov  [edi+8], ax                    ; offset in F000
    mov  word ptr [edi+10], 0F000h
    mov  byte ptr [edi+ROM_PARK_COUNT], 0
    mov  esi, 2                ; two SIPIs at most
pr_sipi:
    mov  eax, [g_tr_phys]
    shr  eax, 12
    or   eax, ICR_ALL_BUT_SELF or ICR_SIPI
    call ipi_once
    mov  ecx, 1
    call delay_ms
    movzx eax, byte ptr [edi+ROM_PARK_COUNT]
    cmp  eax, ebx
    jae  short pr_count
    dec  esi
    jnz  short pr_sipi
    push ebx                   ; wait for the rest, at most ROM_PARK_WAIT_MS
    int  DYNA_LINK_INT
    dd   SVC_Get_System_Time
    mov  esi, eax
    mov  ecx, TIME_POLL_CAP
pr_wait:
    movzx eax, byte ptr [edi+ROM_PARK_COUNT]
    cmp  eax, [esp]
    jae  short pr_waited
    int  DYNA_LINK_INT
    dd   SVC_Get_System_Time
    sub  eax, esi
    cmp  eax, ROM_PARK_WAIT_MS+1
    jae  short pr_waited
    dec  ecx
    jnz  short pr_wait
pr_waited:
    pop  ebx
pr_count:
    movzx eax, byte ptr [edi+ROM_PARK_COUNT]
    mov  [g_rom_parked], eax
pr_done:
    ret
park_in_rom endp

; ---- DIOC_LOCK: input {linear, bytes} in the caller's process. Locks the
;      pages with _LinPageLock (present, PTEs unchanged until unlocked;
;      PAGEMARKDIRTY since the APs write) and records the range for
;      DIOC_UNLOCK. Returns eax = 0 ok, 1 bad input, 2 table full, 3 failed. ----
do_lock proc
    cmp  dword ptr [ebp+DP_CBIN], 8
    jb   short dl_bad
    mov  esi, [ebp+DP_IN]
    test esi, esi
    jz   short dl_bad
    mov  ecx, [esi+4]          ; bytes
    test ecx, ecx
    jz   short dl_bad
    mov  ebx, [esi]            ; first page = linear >> 12
    lea  ecx, [ebx+ecx-1]      ; last page = (linear + bytes - 1) >> 12
    shr  ebx, 12
    shr  ecx, 12
    sub  ecx, ebx
    inc  ecx                   ; pages
    mov  eax, [g_lock_n]
    cmp  eax, MAX_LOCKS
    jae  short dl_full
    push ecx
    push PAGEMARKDIRTY
    push ecx
    push ebx
    int  DYNA_LINK_INT
    dd   SVC_LinPageLock
    add  esp, 12
    pop  ecx
    test eax, eax
    jz   short dl_fail
    mov  eax, [g_lock_n]
    mov  [g_lock_tab+eax*8], ebx
    mov  [g_lock_tab+eax*8+4], ecx
    inc  dword ptr [g_lock_n]
    cmp  eax, 0
    jne  short dl_ok
    shr  ebx, 10               ; first range: remember the context
    mov  [g_lock_ctx_i], ebx
    call lock_ctx_pde
    mov  [g_lock_ctx], eax
dl_ok:
    call refresh_pd
    xor  eax, eax
    ret
dl_bad:
    mov  eax, 1
    ret
dl_full:
    mov  eax, 2
    ret
dl_fail:
    mov  eax, 3
    ret
do_lock endp

; ---- AP page directory: add PDEs 1-511 that became present in the live
;      directory since START (the process grew; LOCK runs in its context).
;      Only not-present AP entries change, which the CPUs never cache, so the
;      running APs need no TLB flush. A present AP entry that differs is left
;      alone and counted (Windows would have replaced a page table). ----
refresh_pd proc
    cmp  dword ptr [g_running], 0
    je   short rp_done
    call map_win_pd
    jc   short rp_done
    mov  esi, eax
    mov  edi, [g_linear]
    mov  ecx, 1
rp_loop:
    mov  eax, [esi+ecx*4]
    test eax, 1
    jz   short rp_next
    mov  edx, [edi+ecx*4]
    test edx, 1
    jnz  short rp_have
    mov  [edi+ecx*4], eax
    inc  dword ptr [g_pd_added]
    jmp  short rp_next
rp_have:
    xor  edx, eax
    test edx, 0FFFFF000h
    jz   short rp_next
    inc  dword ptr [g_pd_clash]
rp_next:
    inc  ecx
    cmp  ecx, 512
    jb   short rp_loop
rp_done:
    ret
refresh_pd endp

; ---- RUN with no job posted or running on any AP: copy all PDEs 1-511
;      from the live directory, also present ones that changed (Windows may
;      have replaced a page table while memory was unlocked between two
;      sessions). Safe only now: no AP walks these tables, and every AP
;      reloads CR3 (flushes its TLB) before its next job. Preserves esi/ecx. ----
resync_pd_if_idle proc
    push esi
    push ecx
    xor  ecx, ecx
rpi_check:
    mov  eax, [g_job_state+ecx*4]
    cmp  eax, JOB_POSTED
    je   short rpi_done
    cmp  eax, JOB_RUNNING
    je   short rpi_done
    inc  ecx
    cmp  ecx, MAX_AP
    jb   short rpi_check
    call map_win_pd
    jc   short rpi_done
    mov  esi, eax
    mov  edx, [g_linear]
    mov  ecx, 1
rpi_copy:
    mov  eax, [esi+ecx*4]
    cmp  eax, [edx+ecx*4]
    je   short rpi_next
    test dword ptr [edx+ecx*4], 1
    jz   short rpi_set
    push eax
    xor  eax, [edx+ecx*4]
    test eax, 0FFFFF000h       ; other page table, not just A/D bits
    pop  eax
    jz   short rpi_set
    inc  dword ptr [g_pd_resync]
rpi_set:
    mov  [edx+ecx*4], eax
rpi_next:
    inc  ecx
    cmp  ecx, 512
    jb   short rpi_copy
rpi_done:
    pop  ecx
    pop  esi
    ret
resync_pd_if_idle endp

; ---- DIOC_UNLOCK with input {linear, bytes}: unlock the recorded range
;      with exactly these pages and drop it from the table (last entry moves
;      into its place). Returns eax = 0 ok, 1 not found. ----
do_unlock_range proc
    mov  esi, [ebp+DP_IN]
    mov  ecx, [esi+4]
    test ecx, ecx
    jz   short dur_nf
    mov  ebx, [esi]
    lea  ecx, [ebx+ecx-1]
    shr  ebx, 12               ; first page
    shr  ecx, 12
    sub  ecx, ebx
    inc  ecx                   ; pages
    mov  edx, [g_lock_n]
dur_find:
    test edx, edx
    jz   short dur_nf
    dec  edx
    cmp  [g_lock_tab+edx*8], ebx
    jne  short dur_find
    cmp  [g_lock_tab+edx*8+4], ecx
    jne  short dur_find
    push edx
    push PAGEMARKDIRTY
    push ecx
    push ebx
    int  DYNA_LINK_INT
    dd   SVC_LinPageUnLock
    add  esp, 12
    pop  edx
    mov  eax, [g_lock_n]
    dec  eax
    mov  [g_lock_n], eax
    mov  ecx, [g_lock_tab+eax*8]
    mov  [g_lock_tab+edx*8], ecx
    mov  ecx, [g_lock_tab+eax*8+4]
    mov  [g_lock_tab+edx*8+4], ecx
    xor  eax, eax
    ret
dur_nf:
    mov  eax, 1
    ret
do_unlock_range endp

; ---- DIOC_UNLOCK: unlock every recorded range. Must run in the context of
;      the process that locked them (linear page numbers are per process), so
;      device exit does not call it. Stop the APs first. ----
do_unlock_all proc
dua_loop:
    mov  eax, [g_lock_n]
    test eax, eax
    jz   short dua_done
    dec  eax
    mov  [g_lock_n], eax
    push PAGEMARKDIRTY
    push dword ptr [g_lock_tab+eax*8+4]
    push dword ptr [g_lock_tab+eax*8]
    int  DYNA_LINK_INT
    dd   SVC_LinPageUnLock
    add  esp, 12
    jmp  short dua_loop
dua_done:
    ret
do_unlock_all endp

; ---- DIOC_RUN: input {ap, fn, arg, fpu cw, mxcsr}. Posts fn(arg) to the
;      AP's slot; the AP polls it (ap_pm32). Returns eax = 0 ok, 1 bad input,
;      2 APs not running / no such AP, 3 slot busy, 4 AP faulted. ----
do_run proc
    cmp  dword ptr [ebp+DP_CBIN], 20
    jb   short dr_bad
    mov  esi, [ebp+DP_IN]
    test esi, esi
    jz   short dr_bad
    cmp  dword ptr [g_running], 0
    je   short dr_noap
    mov  ecx, [esi]
    cmp  ecx, [g_ap_next]      ; APs that took an index (<= MAX_AP)
    jae  short dr_noap
    cmp  ecx, MAX_AP
    jae  short dr_noap
    mov  eax, [g_job_state+ecx*4]
    cmp  eax, JOB_FAULT
    je   short dr_fault
    cmp  eax, JOB_POSTED
    je   short dr_busy
    cmp  eax, JOB_RUNNING
    je   short dr_busy
    call resync_pd_if_idle     ; keeps esi/ecx
    mov  eax, [esi+4]
    mov  [g_job_fn+ecx*4], eax
    mov  eax, [esi+8]
    mov  [g_job_arg+ecx*4], eax
    mov  eax, [esi+12]
    mov  [g_job_fpucw+ecx*4], eax
    mov  eax, [esi+16]
    mov  [g_job_mxcsr+ecx*4], eax
    mov  dword ptr [g_job_ret+ecx*4], 0
    mov  dword ptr [g_job_state+ecx*4], JOB_POSTED   ; last: x86 keeps store order
    xor  eax, eax
    ret
dr_bad:
    mov  eax, 1
    ret
dr_noap:
    mov  eax, 2
    ret
dr_busy:
    mov  eax, 3
    ret
dr_fault:
    mov  eax, 4
    ret
do_run endp

; ---- 32-bit AP landing (entered by the trampoline's far jump). DX = data
;      selector. Takes an AP index n and the stack slot n in the block, then
;      polls the stop flag and its job slot with interrupts off. A job is
;      cdecl int fn(void *arg) in the caller's process (same flat selectors,
;      base 0, so its linear addresses are the ring-3 ones); FS is not the
;      TIB, so job code must not use FS/SEH/TLS. On g_ap_stop the AP counts
;      itself in g_ap_parked and halts until INIT. ----
ap_pm32:
    mov ds, dx
    mov es, dx
    mov ss, dx
    mov fs, dx
    mov gs, dx
    jmp ap_read_id
ap_have_id:                          ; esi = APIC ID
    mov eax, esi
    mov ecx, [g_smt_shift]
    shr eax, cl
    and eax, 0FFh
    cmp eax, [g_bsp_core]
    je  ap_sibling               ; second thread of the BSP's core
    lock bts [g_core_claim], eax
    jc  ap_sibling               ; core already has an AP
    mov eax, 1
    lock xadd [g_ap_next], eax
    cmp eax, MAX_AP
    jb  short ap_have_idx
    lock dec dword ptr [g_ap_next]   ; no slot: undo, park unused
    lock inc dword ptr [g_ap_running]
    jmp ap_park
ap_have_idx:
    mov ebx, eax                     ; ebx = AP index (kept across jobs)
    mov [g_ap_apic+ebx*4], esi
    shl eax, STACK_SHIFT
    add eax, [g_stk_linear]
    add eax, BLK_STACKS+STACK_SIZE
    mov esp, eax
    cld
    clts                             ; CR0 copied from the BSP may carry TS
    lock inc dword ptr [g_ap_running]
ap_loop:
    lock inc dword ptr [g_ap_hb]
    mov ecx, AP_SPIN
ap_wait:
    cmp dword ptr [g_ap_stop], 0     ; checked on every iteration
    jne ap_park
    cmp dword ptr [g_job_state+ebx*4], JOB_POSTED
    je  short ap_job
    db  0F3h, 90h                    ; pause
    dec ecx
    jnz short ap_wait
    jmp short ap_loop
ap_job:
    mov dword ptr [g_job_state+ebx*4], JOB_RUNNING
    mov eax, cr3                     ; flush this core's TLB: Windows changes
    mov cr3, eax                     ;   PTEs of unlocked memory (between two
                                     ;   jobs) and flushes only its own core
    fninit                           ; FPU/SSE settings of the caller
    fldcw word ptr [g_job_fpucw+ebx*4]
    mov eax, cr4
    test eax, 200h                   ; CR4.OSFXSR: SSE enabled by Windows
    jz  short ap_nosse
    db  0Fh, 0AEh, 14h, 9Dh          ; ldmxcsr [g_job_mxcsr+ebx*4]
    dd  offset g_job_mxcsr
ap_nosse:
    mov esp, ebx                     ; fresh stack for every job
    shl esp, STACK_SHIFT
    add esp, [g_stk_linear]
    add esp, BLK_STACKS+STACK_SIZE
    push ebx
    push dword ptr [g_job_arg+ebx*4]
    call dword ptr [g_job_fn+ebx*4]
    add esp, 4
    pop ebx
    mov [g_job_ret+ebx*4], eax
    mov dword ptr [g_job_state+ebx*4], JOB_DONE
    jmp short ap_loop
ap_sibling:                          ; hyperthread: no worker (shares the
    lock inc dword ptr [g_ap_sibling]  ; core's execution units), parks now
    lock inc dword ptr [g_ap_running]
ap_park:
    lock inc dword ptr [g_ap_parked]
ap_halt:
    cli
    hlt
    jmp short ap_halt

; APIC ID on an AP without a stack (same as apic_id_eax) -> esi
ap_read_id:
    cmp  dword ptr [g_max_leaf], 0Bh
    jb   short ari_leaf1
    mov  eax, 0Bh
    xor  ecx, ecx
    cpuid
    test ebx, 0FFFFh
    jz   short ari_leaf1
    mov  esi, edx
    jmp  ap_have_id
ari_leaf1:
    mov  eax, 1
    cpuid
    mov  esi, ebx
    shr  esi, 24
    jmp  ap_have_id

; ---- exception stubs (EXC_STUB_SIZE bytes each, IDT order): push the
;      vector, jump to exc_common. Fixed encoding so build_idt can index. ----
exc_stubs:
EXC_N = 0
rept EXC_VECTORS
    db  6Ah, EXC_N                   ; push imm8
    db  0E9h                         ; jmp rel32 exc_common
    dd  exc_common - ($ + 4)
EXC_N = EXC_N + 1
endm

; Record the fault in the slot of the AP that owns this stack (index from
; ESP; jobs run on the block stacks only), mark the job JOB_FAULT, park.
; Stack: vector, [error code for 8, 10-14, 17], EIP, CS, EFLAGS.
exc_common:
    pop  eax                         ; vector
    mov  edx, esp
    sub  edx, [g_stk_linear]
    sub  edx, BLK_STACKS
    shr  edx, STACK_SHIFT            ; AP index
    cmp  edx, MAX_AP
    jae  short ec_park
    mov  [g_fault_vec+edx*4], eax
    cmp  eax, 8
    je   short ec_err
    cmp  eax, 17
    je   short ec_err
    cmp  eax, 10
    jb   short ec_noerr
    cmp  eax, 14
    ja   short ec_noerr
ec_err:
    add  esp, 4                      ; drop the error code
ec_noerr:
    mov  ecx, [esp]                  ; faulting EIP
    mov  [g_fault_eip+edx*4], ecx
    mov  ecx, cr2
    mov  [g_fault_cr2+edx*4], ecx
    mov  dword ptr [g_job_state+edx*4], JOB_FAULT
ec_park:
    lock inc dword ptr [g_ap_faults]
    jmp  ap_park
_LTEXT ends
end
