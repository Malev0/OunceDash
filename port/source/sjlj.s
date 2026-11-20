// bionic sigsetjmp/siglongjmp: signal masks do not exist here, so these
// are plain setjmp/longjmp. Tail calls keep the caller's frame, which
// setjmp must see.
    .text
    .global b_sigsetjmp
    .type b_sigsetjmp, %function
b_sigsetjmp:
    b setjmp

    .global b_siglongjmp
    .type b_siglongjmp, %function
b_siglongjmp:
    b longjmp
