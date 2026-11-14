from PIL import Image, ImageDraw, ImageFont
import sys
W,H=1800,760
im=Image.new("RGB",(W,H),"white"); d=ImageDraw.Draw(im)
F=lambda n:ImageFont.truetype(r"C:\Windows\Fonts\segoeui.ttf",n)
fb=F(30); fs=F(23); ft=F(34)
def box(x,y,w,h,txt,sub=None,fill="#e8f0fe",out="#1a4fa0"):
    d.rounded_rectangle((x,y,x+w,y+h),12,fill=fill,outline=out,width=3)
    tw=d.textlength(txt,font=fb); d.text((x+(w-tw)/2,y+(14 if sub else h/2-18)),txt,font=fb,fill="black")
    if sub:
        for i,l in enumerate(sub.split("\n")):
            tw=d.textlength(l,font=fs); d.text((x+(w-tw)/2,y+58+i*28),l,font=fs,fill="#333")
def arrow(p,q,col="black"):
    d.line((p,q),fill=col,width=3)
    import math
    a=math.atan2(q[1]-p[1],q[0]-p[0]); L=16
    for s in (0.45,-0.45):
        d.line((q,(q[0]-L*math.cos(a-s),q[1]-L*math.sin(a-s))),fill=col,width=3)
d.text((40,25),"Головний цикл while (1) { app_poll(); }",font=ft,fill="black")
names=[("cli","USART2 RX"),("event","кнопка"),("imu","20 мс"),("led","500/100 мс"),("tlm","200 мс"),("jrn","100 мс"),("lop","1 с")]
x0,y0,w,h,g=40,120,205,110,32
xs=[]
for i,(n,s) in enumerate(names):
    x=x0+i*(w+g); xs.append(x)
    box(x,y0,w,h,n,s)
    if i: arrow((x-g,y0+h/2),(x,y0+h/2))
# end box
bx=xs[-1]; 
d.line(((xs[-1]+w/2,y0+h),(xs[-1]+w/2,y0+h+60),(xs[0]+w/2,y0+h+60),(xs[0]+w/2,y0+h+8)),fill="black",width=3,joint="curve")
arrow((xs[0]+w/2,y0+h+40),(xs[0]+w/2,y0+h+2))
d.text((xs[0]+w/2+20,y0+h+72),"наступна ітерація: recover, load, перевірка кінця сценарію",font=fs,fill="#333")
# ISR layer
yi=500
d.text((40,yi-55),"Переривання: лише кладуть дані в буфер/чергу, задачі їх читають",font=fb,fill="black")
box(40,yi,300,110,"USART2 ISR","байт → кільцевий буфер",fill="#fff4e0",out="#b36b00")
box(400,yi,300,110,"EXTI4_15 (PC13)","мітка часу → черга подій",fill="#fff4e0",out="#b36b00")
box(760,yi,300,110,"SysTick 1 мс","лічильник часу, jitter",fill="#fff4e0",out="#b36b00")
box(1120,yi,420,110,"TIM3 PWM","сервоприводи, імпульс 1–2 мс / 20 мс",fill="#e6f6ea",out="#1f7a3a")
d.text((1120,yi+135),"PB6 – вікно imu, PB7 – довга операція,",font=fs,fill="#333")
d.text((1120,yi+165),"PA1 – реакція на подію, PB2 – стимул",font=fs,fill="#333")
im.save(sys.argv[1])

