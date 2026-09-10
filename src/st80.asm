OPTION CASEMAP:NONE

.code

PUBLIC st80_to_double
PUBLIC double_to_st80
PUBLIC st80_to_float

; double st80_to_double(const uint8_t *bytes, uint16_t fpu_control_word)
; Rounds an 80-bit x87 value to double using the supplied rounding mode.
st80_to_double PROC
    fnstcw  word ptr[rsp + 8]
    mov     ax, word ptr[rsp + 8]
    and     ax, 0F3FFh
    and     dx, 0C00h
    or      ax, dx
    mov     word ptr[rsp + 10], ax
    fldcw   word ptr[rsp + 10]
    fld     tbyte ptr[rcx]
    fstp    qword ptr[rsp + 16]
    fldcw   word ptr[rsp + 8]
    movsd   xmm0, qword ptr[rsp + 16]
    ret
st80_to_double ENDP

; void double_to_st80(double value, uint8_t *out)
; Converts a double to an 80-bit x87 value on the FPU.
double_to_st80 PROC
    movsd   qword ptr[rsp + 8], xmm0
    fld     qword ptr[rsp + 8]
    fstp    tbyte ptr[rdx]
    ret
double_to_st80 ENDP

; float st80_to_float(const uint8_t *bytes, uint16_t fpu_control_word)
; Rounds an 80-bit x87 value directly to float using the supplied rounding mode.
st80_to_float PROC
    fnstcw  word ptr[rsp + 8]
    mov     ax, word ptr[rsp + 8]
    and     ax, 0F3FFh
    and     dx, 0C00h
    or      ax, dx
    mov     word ptr[rsp + 10], ax
    fldcw   word ptr[rsp + 10]
    fld     tbyte ptr[rcx]
    fstp    dword ptr[rsp + 16]
    fldcw   word ptr[rsp + 8]
    movss   xmm0, dword ptr[rsp + 16]
    ret
st80_to_float ENDP

END
