// SPDX-FileCopyrightText: 2026 jiri-god
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "nodes/looperengine.hpp"
#include <element/juce/gui_basics.hpp>
#include <optional>

namespace element {
/** Message-thread keyboard pedal latch. A physical release rearms each key. */
class LooperKeyboard
{
public:
    struct Result
    {
        bool consumed = false;
        std::optional<LooperEngine::Action> action;
    };
    static constexpr std::array<int, 6> keys { 'A', 'S', 'D', 'Z', 'X', 'C' };

    /** Text entry and modified application commands retain their normal handling. */
    static bool allows (const juce::KeyPress& key, const juce::Component* focused, const juce::Component& editor)
    {
        const auto modifiers = key.getModifiers();
        if (modifiers.isCommandDown() || modifiers.isCtrlDown() || modifiers.isAltDown()
            || focused == nullptr || (focused != &editor && ! editor.isParentOf (focused)))
            return false;
        for (auto* component = focused; component != nullptr; component = component->getParentComponent())
        {
            if (dynamic_cast<const juce::TextEditor*> (component) != nullptr)
                return false;
            if (const auto* label = dynamic_cast<const juce::Label*> (component))
                if (label->isBeingEdited())
                    return false;
            if (component == &editor)
                break;
        }
        return true;
    }
    Result press (int code, bool allowed, bool shift = false) noexcept
    {
        if (code >= 'a' && code <= 'z')
            code -= 'a' - 'A';
        for (int i = 0; i < 6; ++i)
        {
            if (code != keys[i])
                continue;
            const bool first = ! held[i];
            held[i] = true;
            Result result;
            allowed = allowed && (code != 'C' || shift);
            result.consumed = allowed;
            if (allowed && first)
            {
                constexpr LooperEngine::Action actions[] { LooperEngine::Action::overdub, LooperEngine::Action::record, LooperEngine::Action::playStop, LooperEngine::Action::undo, LooperEngine::Action::redo, LooperEngine::Action::clear };
                result.action = actions[i];
            }
            return result;
        }
        return {};
    }
    /** Entering/re-entering editor focus while a key is held must not fire it. */
    void suppressHeldKeys (const std::array<bool, 6>& down) noexcept
    {
        for (int i = 0; i < 6; ++i)
            if (down[i])
                held[i] = true;
    }
    /** Update on releases and timer ticks; never turn a key-down into another press. */
    void releaseUpKeys (const std::array<bool, 6>& down) noexcept
    {
        for (int i = 0; i < 6; ++i)
            if (! down[i])
                held[i] = false;
    }

private:
    std::array<bool, 6> held {};
};
} // namespace element
