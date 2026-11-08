// SPDX-License-Identifier: GPL-3.0-or-later
#include "compact-ui.mm"
#include <cassert>
static float written=0;static int writes=0;
static void writeValue(LV2UI_Controller,uint32_t port,uint32_t size,uint32_t format,const void* data){assert(port==5 && size==4 && format==0);std::memcpy(&written,data,4);++writes;}
int main(){@autoreleasepool {
    [NSApplication sharedApplication];
    float cents=99;assert([pitchText(440,440,&cents) isEqualToString:@"A4"] && std::abs(cents)<.01);
    assert([pitchText(30.8677f,440,&cents) isEqualToString:@"B0"] && std::abs(cents)<.1);
    assert([pitchText(41.2034f,440,&cents) isEqualToString:@"E1"]);
    assert([pitchText(0,440) isEqualToString:@"No signal"]);
    auto* descriptor=lv2ui_descriptor(0);assert(descriptor && !lv2ui_descriptor(1));
    NSView* parent=[[NSView alloc] initWithFrame:NSMakeRect(0,0,0,0)];LV2_Feature parentFeature{LV2_UI__parent,parent};const LV2_Feature* features[]{&parentFeature,nullptr};
    LV2UI_Widget widget=nullptr;auto handle=descriptor->instantiate(descriptor,"http://gareus.org/oss/lv2/tuna#one","",writeValue,nullptr,&widget,features);
    float hz=41.2034f;descriptor->port_event(handle,7,4,0,&hz);
    static_cast<const LV2UI_Idle_Interface*>(descriptor->extension_data(LV2_UI__idleInterface))->idle(handle);
    auto* view=(JGodTunaCompactView*)widget;assert(view.superview==parent);assert(parent.bounds.size.width==420);assert(view.bounds.size.width==420 && view.bounds.size.height==70);
    assert([[view accessibilityValue] hasPrefix:@"E1"]);
    view->referenceField.stringValue=@"432";[view referenceChanged:nil];assert(writes==1 && written==432);
    view->referenceField.stringValue=@"999";[view referenceChanged:nil];assert(writes==1 && view->values->reference.load()==432);
    view->referenceField.stringValue=@"wrong";[view referenceChanged:nil];assert(writes==1);
    float ref=442;descriptor->port_event(handle,5,4,0,&ref);static_cast<const LV2UI_Idle_Interface*>(descriptor->extension_data(LV2_UI__idleInterface))->idle(handle);
    assert(view->referenceField.floatValue==442);
    view->referenceField.stringValue=@"440";[view referenceChanged:nil];assert(written==440);
    NSBitmapImageRep* bitmap=[view bitmapImageRepForCachingDisplayInRect:view.bounds];[view cacheDisplayInRect:view.bounds toBitmapImageRep:bitmap];
    [[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@"/tmp/tuna-compact.png" atomically:YES];
    descriptor->cleanup(handle);assert(parent.subviews.count==0);[parent release];puts("Compact tuner: pitch/ports/lifecycle/render OK");
}}
