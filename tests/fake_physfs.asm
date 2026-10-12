; Copies of the two PhysFS signatures from src/signatures.h, so engine_probe can resolve them
; inside the smoke-test exe. Never executed.
.code

fake_doOpen PROC
    db 040h,055h,041h,056h,041h,057h,048h,083h,0ECh,040h,048h,08Dh,06Ch,024h,040h,048h,089h,05Dh,020h,045h
    db 08Bh,0F0h,048h,089h,075h,028h,044h,08Bh,0FAh,048h,089h,07Dh,030h,048h,08Bh,0D9h,048h,085h,0C9h,00Fh,084h
    db 0, 0, 0, 0
    ret
fake_doOpen ENDP

fake_openRead PROC
    db 048h,083h,0ECh,028h,0BAh,000h,000h,000h,080h,041h,0B8h,003h,000h,000h,000h
    call fake_doOpen
    db 033h,0C9h,048h,083h,0F8h,0FFh,048h,00Fh,044h,0C1h,048h,083h,0C4h,028h,0C3h
fake_openRead ENDP

END
