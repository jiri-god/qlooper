// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
// A Cocoa LV2 UI for the existing x42 Instrument Tuner. No pitch detection/DSP.
#import <Cocoa/Cocoa.h>
#include <lv2/ui/ui.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>

struct TunerValues {
    std::atomic<float> frequency{0}, reference{440};
    LV2UI_Write_Function write=nullptr;LV2UI_Controller controller=nullptr;
};
static NSString* pitchText(float hz,float reference,float* cents=nullptr) {
    if(!std::isfinite(hz)||hz<20||hz>10000||!std::isfinite(reference)||reference<220||reference>880)return @"No signal";
    const double pitch=69+12*std::log2(hz/reference);const int note=int(std::lround(pitch));
    if(cents)*cents=float((pitch-note)*100);
    static const char* names[]={"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    return [NSString stringWithFormat:@"%s%d",names[(note%12+12)%12],note/12-1];
}
@interface JGodTunaReferenceField : NSTextField
@end
@implementation JGodTunaReferenceField
- (void)mouseDown:(NSEvent*)event {
    if(event.clickCount==2){self.stringValue=@"440";[self sendAction:self.action to:self.target];return;}
    [super mouseDown:event];[self selectText:nil];
}
@end
@interface JGodTunaCompactView : NSView {
@public TunerValues* values; JGodTunaReferenceField* referenceField;
}
@end
@implementation JGodTunaCompactView
- (BOOL)isFlipped {return YES;}
- (BOOL)isOpaque {return YES;}
- (BOOL)isAccessibilityElement {return NO;}
- (NSString*)accessibilityLabel {return @"Chromatic instrument tuner";}
- (id)accessibilityValue {
    float cents=0;NSString* note=pitchText(values->frequency.load(),values->reference.load(),&cents);
    return [NSString stringWithFormat:@"%@, %.1f cents",note,cents];
}
- (void)referenceChanged:(id)sender {
    (void)sender;NSScanner* scanner=[NSScanner scannerWithString:referenceField.stringValue];double v=0;
    if([scanner scanDouble:&v] && scanner.isAtEnd && std::isfinite(v) && v>=220 && v<=880){
        const float next=float(v);values->reference.store(next);
        if(values->write)values->write(values->controller,5,sizeof(next),0,&next);
    }
    referenceField.stringValue=[NSString stringWithFormat:@"%.1f",values->reference.load()];[self setNeedsDisplay:YES];
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;const NSRect b=self.bounds;
    [[NSColor colorWithCalibratedRed:.125 green:.153 blue:.192 alpha:1] setFill];NSRectFill(b);
    const float hz=values->frequency.load(),ref=values->reference.load();float cents=0;
    NSString* note=pitchText(hz,ref,&cents);const bool valid=![note isEqualToString:@"No signal"];
    NSDictionary* text=@{NSFontAttributeName:[NSFont systemFontOfSize:13],NSForegroundColorAttributeName:[NSColor whiteColor]};
    NSDictionary* big=@{NSFontAttributeName:[NSFont monospacedDigitSystemFontOfSize:valid?32:18 weight:NSFontWeightMedium],NSForegroundColorAttributeName:[NSColor whiteColor]};
    [note drawAtPoint:NSMakePoint(12,valid?13:22) withAttributes:big];
    const CGFloat x=115,w=MAX(80,b.size.width-x-12),cy=32;
    [[NSColor colorWithCalibratedWhite:.65 alpha:1] setStroke];NSBezierPath* ticks=[NSBezierPath bezierPath];
    for(int i=0;i<=10;++i){const CGFloat px=x+w*i/10.;[ticks moveToPoint:NSMakePoint(px,cy-(i==5?12:6))];[ticks lineToPoint:NSMakePoint(px,cy+(i==5?12:6))];}[ticks stroke];
    if(valid){const CGFloat px=x+w*(.5+std::fmax(-50,std::fmin(50,cents))/100.);
        [(std::abs(cents)<=5?[NSColor colorWithCalibratedRed:.5 green:.9 blue:.65 alpha:1]:[NSColor colorWithCalibratedRed:1 green:.7 blue:.35 alpha:1]) setFill];
        NSRectFill(NSMakeRect(px-2,cy-13,4,26));}
    NSString* footer=valid?[NSString stringWithFormat:@"%+.1f ct   %.2f Hz",cents,hz]:@"x42 Instrument Tuner";
    [footer drawAtPoint:NSMakePoint(x,49) withAttributes:text];
    [@"A" drawAtPoint:NSMakePoint(302,49) withAttributes:text];[@"Hz" drawAtPoint:NSMakePoint(390,49) withAttributes:text];
    [@"−50" drawAtPoint:NSMakePoint(x,2) withAttributes:text];[@"0" drawAtPoint:NSMakePoint(x+w/2-4,2) withAttributes:text];[@"+50" drawAtPoint:NSMakePoint(x+w-26,2) withAttributes:text];
}
@end
struct CompactUI {TunerValues values;JGodTunaCompactView* view=nil;float lastHz=-1,lastRef=-1;};
static LV2UI_Handle instantiate(const LV2UI_Descriptor*,const char*,const char*,LV2UI_Write_Function write,LV2UI_Controller controller,LV2UI_Widget* widget,const LV2_Feature* const* features) {
    auto* ui=new CompactUI;ui->values.write=write;ui->values.controller=controller;
    ui->view=[[JGodTunaCompactView alloc] initWithFrame:NSMakeRect(0,0,420,70)];ui->view->values=&ui->values;
    auto* field=[[JGodTunaReferenceField alloc] initWithFrame:NSMakeRect(318,46,66,22)];
    field.stringValue=@"440.0";field.font=[NSFont systemFontOfSize:13];field.textColor=[NSColor whiteColor];
    field.backgroundColor=[NSColor colorWithCalibratedWhite:.08 alpha:1];field.bordered=YES;
    field.target=ui->view;field.action=@selector(referenceChanged:);field.accessibilityLabel=@"Reference pitch A4 in Hz";
    field.toolTip=@"Reference pitch, 220–880 Hz. Enter to apply; double-click restores 440 Hz. Saved with the session.";
    [ui->view addSubview:field];ui->view->referenceField=field;[field release];*widget=(void*)ui->view;
    if(features)for(auto f=features;*f;++f)if(std::strcmp((*f)->URI,LV2_UI__parent)==0 && (*f)->data){
        NSView* parent=(NSView*)(*f)->data;[parent setFrameSize:NSMakeSize(420,70)];[parent addSubview:ui->view];break;
    }
    return ui;
}
static void cleanup(LV2UI_Handle handle){auto* ui=static_cast<CompactUI*>(handle);[ui->view removeFromSuperview];[ui->view release];delete ui;}
static void portEvent(LV2UI_Handle handle,uint32_t port,uint32_t size,uint32_t format,const void* data){
    if(format||size!=sizeof(float)||!data)return;float v;std::memcpy(&v,data,sizeof(v));auto& values=static_cast<CompactUI*>(handle)->values;
    if(port==7)values.frequency.store(v);else if(port==5)values.reference.store(v);
}
static int idle(LV2UI_Handle handle){auto* ui=static_cast<CompactUI*>(handle);float hz=ui->values.frequency.load(),ref=ui->values.reference.load();if(hz!=ui->lastHz||ref!=ui->lastRef){ui->lastHz=hz;ui->lastRef=ref;if(![ui->view->referenceField currentEditor])ui->view->referenceField.stringValue=[NSString stringWithFormat:@"%.1f",ref];[ui->view setNeedsDisplay:YES];}return 0;}
static const void* extension(const char* uri){static const LV2UI_Idle_Interface iface{idle};return std::strcmp(uri,LV2_UI__idleInterface)==0?&iface:nullptr;}
extern "C" __attribute__((visibility("default"))) const LV2UI_Descriptor* lv2ui_descriptor(uint32_t index){
    static const LV2UI_Descriptor descriptor{"http://gareus.org/oss/lv2/tuna#ui_gl",instantiate,cleanup,portEvent,extension};return index==0?&descriptor:nullptr;
}
