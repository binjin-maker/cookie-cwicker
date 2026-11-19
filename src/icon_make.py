from PIL import Image, ImageDraw, ImageFilter
S=1024
im=Image.new("RGBA",(S,S),(0,0,0,0)); d=ImageDraw.Draw(im)
cx,cy,R=512,575,385
# antenna
d.line([(512,cy-R+20),(512,105)],fill=(70,80,95),width=34)
d.ellipse([452,50,572,170],fill=(235,60,60)); d.ellipse([480,72,520,112],fill=(255,170,160))
# ear bolts
for x in (cx-R-40,cx+R-40):
    d.rounded_rectangle([x,cy-70,x+80,cy+70],radius=22,fill=(90,100,115))
# cookie body
d.ellipse([cx-R+18,cy-R+28,cx+R+18,cy+R+28],fill=(120,70,30))  # shadow
d.ellipse([cx-R,cy-R,cx+R,cy+R],fill=(214,150,78))
d.ellipse([cx-R+40,cy-R+40,cx+R-40,cy+R-40],fill=(226,166,92))
# chips
for (x,y,r) in [(300,390,38),(720,400,34),(250,720,40),(760,760,36),(520,880,32),(650,300,26),(380,300,24)]:
    d.ellipse([x-r,y-r,x+r,y+r],fill=(85,45,20))
# visor
d.rounded_rectangle([cx-270,cy-150,cx+270,cy+30],radius=80,fill=(25,30,48))
for ex in (cx-130,cx+130):
    g=Image.new("RGBA",(S,S),(0,0,0,0)); gd=ImageDraw.Draw(g)
    gd.ellipse([ex-75,cy-130,ex+75,cy+10],fill=(60,200,255,200)); g=g.filter(ImageFilter.GaussianBlur(28)); im.alpha_composite(g)
    d=ImageDraw.Draw(im); d.ellipse([ex-52,cy-112,ex+52,cy-8],fill=(120,225,255)); d.ellipse([ex-24,cy-92,ex+6,cy-62],fill=(235,250,255))
# grill mouth
d.rounded_rectangle([cx-150,cy+100,cx+150,cy+190],radius=30,fill=(60,65,80))
for i in range(5):
    x=cx-110+i*55; d.rectangle([x,cy+115,x+18,cy+175],fill=(150,160,175))
im.save("big.png")
sizes=[16,24,32,48,64,128,256]
im.resize((256,256),Image.LANCZOS).save("cookiebot.ico",sizes=[(s,s) for s in sizes])
im.resize((256,256),Image.LANCZOS).save("preview.png")
