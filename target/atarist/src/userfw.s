; Sidepad user firmware: inject the BT controller as Atari ST joystick 1, and
; (when mouse mode is on) the right stick as the GEM mouse via the IKBD mousevec.
; (C) 2026 Neil Rackett
; License: GPL v3
;
; Called once (via main.s rom_function) when the RP raises CMD_START.
;
; Mechanism
; ---------
; The RP writes a packed controller state byte to shared-variable slot 3
; every loop (bit0=Up, bit1=Down, bit2=Left, bit3=Right, bit6=Fire - the standard
; IKBD encoding, confirmed on hardware via JOYMOUT). A handler hooked onto the VBL autovector
; ($70) watches that byte and, when it changes, synthesises a TOS joystick packet
; [$FF, joy0, joy1] (our state in the joy1 byte) and calls the system joyvec. The
; real joyvec is left in place, so a physical joystick on port 1 keeps working
; alongside the controller — whichever one is moved drives joystick 1.
;
; Why hook $70 and not _vblqueue
; ------------------------------
; A free _vblqueue slot works during boot but GEM reclaims it for its own VBL
; routines when the desktop loads, silently de-linking us. The $70 autovector is
; the OS's VBL entry point and GEM does not replace it (it only adds/removes
; _vblqueue entries, which the $70 handler walks), so a hook there survives into
; the desktop. We chain to the saved original so the OS VBL processing still runs.
;
; Why copy to RAM
; ---------------
; The whole cartridge region ($FA0000-$FAFFFF) is emulated read-only to the
; ST: code can be fetched/read from it, but stores bus-error. The resident
; handler needs writable state (saved joyvec, previous sample, packet
; buffer), so the installer Malloc's a RAM block and copies the resident,
; position-independent handler into it. Reads of the shared region (the
; controller byte) still work, so the handler reads slot 3 in place.

    section text

GEMDOS_Malloc       equ 72          ; trap #1
XBIOS_Kbdvbase      equ 34          ; trap #14 -> KBDVECS pointer in d0
KBDVECS_JOYVEC_OFF  equ 24          ; joyvec field within KBDVECS (offset 24!)
KBDVECS_MOUSEVEC_OFF equ 16         ; mousevec field within KBDVECS
VBL_VECTOR          equ $70         ; .l  level-4 (VBL) autovector
BT_JOY_BYTE         equ $FA201F     ; LSB of shared-var slot 3 (RP writes it)
; Mouse packet in shared-var slot 5 ($FA2024), one byte each (RP writes them).
; move.l reads the slot big-endian, so byte0 is at the lowest address:
BT_MOUSE_ENABLED    equ $FA2024     ; mouse mode on (1) / off (0)
BT_MOUSE_BTN        equ $FA2025     ; IKBD button bits (bit1 = left button / R3)
BT_MOUSE_DX         equ $FA2026     ; signed per-frame dx
BT_MOUSE_DY         equ $FA2027     ; signed per-frame dy

; -----------------------------------------------------------------------
; Installer — runs from ROM at USERFW ($FA0800). Only writes to RAM.
; -----------------------------------------------------------------------
; a3 holds the RAM block base across the trap calls: GEMDOS/XBIOS may clobber
; d0-d2/a0-a2, but preserve d3-d7/a3-a6, so a3 survives the traps below.
userfw:
    movem.l d0-d2/a0-a3, -(sp)

    ; Malloc a RAM block for the resident handler + its state.
    move.l  #(resident_end-resident_start), -(sp)
    move.w  #GEMDOS_Malloc, -(sp)
    trap    #1
    addq.l  #6, sp
    tst.l   d0
    beq.s   .uf_exit                ; out of memory -> install nothing
    move.l  d0, a3                  ; a3 = RAM block base (survives traps)

    ; Copy the resident code+data (in ROM) into the RAM block.
    lea     resident_start(pc), a0  ; PC in ROM -> ROM source address
    move.l  a3, a1                  ; a1 = destination
    move.w  #(resident_end-resident_start-1), d1
.uf_copy:
    move.b  (a0)+, (a1)+
    dbf     d1, .uf_copy

    ; Save the ADDRESS of the KBDVECS joyvec slot (kbdvecs+24) into the block.
    ; The VBL handler dereferences this live each frame, so it always chains to
    ; whatever joyvec is currently installed -- a joystick reader (game, test
    ; tool) typically installs its own joyvec AFTER us, and a saved snapshot
    ; would bypass it.
    move.w  #XBIOS_Kbdvbase, -(sp)
    trap    #14
    addq.l  #2, sp                  ; d0 = KBDVECS pointer
    move.l  d0, a0
    lea     KBDVECS_JOYVEC_OFF(a0), a0
    move.l  a0, (res_joyslot-resident_start)(a3)
    ; Save the mousevec slot address too (right-stick-as-mouse). Same live-read
    ; rationale as joyvec: a reader may install its own mousevec after us.
    move.l  d0, a0
    lea     KBDVECS_MOUSEVEC_OFF(a0), a0
    move.l  a0, (res_mouseslot-resident_start)(a3)

    ; Hook the VBL autovector ($70): save the original into the block and point
    ; $70 at our resident handler (block offset 0), which chains back to it. Mask
    ; interrupts across the swap so a VBL can't fire mid-update. $70 lives in the
    ; supervisor-only vector page; the installer runs supervisor at boot, so the
    ; access is legal.
    move.w  sr, -(sp)
    ori.w   #$0700, sr             ; raise IPL to 7 (block interrupts)
    move.l  VBL_VECTOR.w, (orig_vbl-resident_start)(a3)
    move.l  a3, VBL_VECTOR.w
    move.w  (sp)+, sr

    ; The exit banner is printed later by main.s boot_gem (from the RP-composed
    ; shared-region string), not here -- printing from the installer would land
    ; back in the terminal print loop and be repainted over. This routine only
    ; installs the joystick hook.

.uf_exit:
    movem.l (sp)+, d0-d2/a0-a3
    rts

; -----------------------------------------------------------------------
; Resident block — copied verbatim into RAM and run from there. Must be
; position independent: it references its own data PC-relative and the
; shared controller byte by absolute address. Entry (offset 0) is installed at
; the VBL autovector ($70): it runs at VBL interrupt level, preserves every
; register it touches, and tail-chains into the saved original $70 handler so
; the OS's normal VBL processing still happens.
; -----------------------------------------------------------------------
    even
resident_start:
    ; Save the FULL register set, not just our scratch: we jsr into the system
    ; joyvec/mousevec, and mousevec (line-A / AES cursor machinery) can clobber
    ; d3-d7/a3-a6. Restoring only d0-d2/a0-a2 would leak that corruption back into
    ; the interrupted program -> intermittent bombs.
    movem.l d0-d7/a0-a6, -(sp)
    move.b  BT_JOY_BYTE, d0         ; current controller state (read OK)
    lea     res_prev(pc), a1
    cmp.b   (a1), d0
    beq.s   .res_mouse              ; unchanged -> skip joystick, try mouse
    move.b  d0, (a1)                ; remember new state

    ; Translate to an IKBD joystick byte: the direction bits (0-3) already match
    ; the IKBD encoding and pass through unchanged; move our fire (bit 6) to the
    ; IKBD fire position (bit 7).
    move.b  d0, d1
    and.b   #$0F, d1
    btst    #6, d0
    beq.s   .res_nofire
    bset    #7, d1
.res_nofire:
    ; Build a real IKBD joystick packet and call the CURRENT joyvec (read live
    ; from the KBDVECS slot). TOS delivers joystick packets as three bytes
    ; [$FF, joystick0, joystick1] with the $FF header at an ODD address, so a
    ; consumer that reads `move.w 1(a0)` (e.g. PP's JOYMOUT tester) gets joy0/joy1
    ; as an aligned word. We drive joystick 1, so our state goes in the joy1 byte
    ; and joy0 is left idle. res_pkt is even-aligned, so res_pkt+1 is the odd
    ; header byte we hand to the vector.
    lea     res_pkt(pc), a0
    move.b  #$FF, 1(a0)             ; header at odd address (res_pkt+1)
    clr.b   2(a0)                   ; joystick 0 = idle
    move.b  d1, 3(a0)              ; joystick 1 = our state
    move.l  res_joyslot(pc), a1     ; address of the KBDVECS joyvec slot
    move.l  (a1), d2                ; current joyvec (live read)
    beq.s   .res_mouse              ; no handler installed -> skip to mouse
    move.l  d2, a1
    addq.l  #1, a0                  ; a0 -> $FF header (odd address)
    ; Block the IKBD ACIA interrupt (IPL 6) across the call, same as the mousevec
    ; call below: driving joyvec from the VBL (IPL 4) leaves the ACIA unmasked, so
    ; a real IKBD packet could otherwise re-enter joyvec on top of us.
    move.w  sr, -(sp)
    ori.w   #$0700, sr
    jsr     (a1)                    ; joyvec convention: a0 -> [$FF, joy0, joy1]
    move.w  (sp)+, sr

.res_mouse:
    ; --- Mouse: while enabled, inject a relative IKBD mouse packet every frame
    ; the stick is deflected (relative deltas accumulate) or the button changed.
    ; Calls the live mousevec, so GEM's cursor (read via graf_mkstate) moves.
    tst.b   BT_MOUSE_ENABLED
    beq.s   .res_chain              ; mouse mode off -> just chain on
    move.b  BT_MOUSE_BTN, d0        ; IKBD buttons (bit1 = left = R3)
    move.b  BT_MOUSE_DX, d1         ; signed dx
    move.b  BT_MOUSE_DY, d2         ; signed dy
    tst.b   d1
    bne.s   .res_domouse            ; moving in x -> inject
    tst.b   d2
    bne.s   .res_domouse            ; moving in y -> inject
    lea     res_mprev(pc), a1       ; idle: inject only if the button changed
    cmp.b   (a1), d0
    beq.s   .res_chain
.res_domouse:
    lea     res_mprev(pc), a1
    move.b  d0, (a1)                ; remember button state
    or.b    #$F8, d0               ; relative-mouse header: $F8 | buttons
    lea     res_mpkt(pc), a0
    move.b  d0, (a0)               ; header
    move.b  d1, 1(a0)             ; dx
    move.b  d2, 2(a0)             ; dy
    move.l  res_mouseslot(pc), a1   ; address of the KBDVECS mousevec slot
    move.l  (a1), d0                ; current mousevec (live read)
    beq.s   .res_chain              ; no handler installed -> skip
    move.l  d0, a1
    ; Block the IKBD ACIA interrupt (IPL 6) across the call: we drive mousevec
    ; from the VBL (IPL 4), which leaves the ACIA unmasked, so a real IKBD packet
    ; could otherwise re-enter mousevec on top of us and corrupt its state.
    move.w  sr, -(sp)
    ori.w   #$0700, sr
    jsr     (a1)                    ; mousevec convention: a0 -> [header, dx, dy]
    move.w  (sp)+, sr

.res_chain:
    movem.l (sp)+, d0-d7/a0-a6
    move.l  orig_vbl(pc), -(sp)     ; chain into the original VBL handler...
    rts                             ; ...which rte's, popping the interrupt frame

    even
res_prev:
    dc.b    0
    even
res_pkt:
    dc.b    0,0,0,0
    even
res_joyslot:
    dc.l    0
    even
res_mprev:
    dc.b    0
    even
res_mpkt:
    dc.b    0,0,0
    even
res_mouseslot:
    dc.l    0
    even
orig_vbl:
    dc.l    0
resident_end:
    even
