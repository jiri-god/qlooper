// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <element/node.hpp>
#include <element/juce/gui_basics.hpp>
namespace element {
class GuiService;
/** A presentation-only composition of the regular node editors. */
class LooperRigView final : public juce::Component {
public:
    explicit LooperRigView(std::function<Node()> root, GuiService* gui=nullptr);
    ~LooperRigView() override;
    void resized() override;
    void refresh();
    void browse(int delta);
    void setViewMode(int mode); // 0 split, 1 loopers, 2 layers
    Node displayedLooper() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
