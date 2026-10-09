// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#include "SlotStore.h"

namespace lift {

juce::File SlotStore::defaultFolder() {
    juce::File base = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
#if JUCE_MAC
    base = base.getChildFile("Application Support");
#endif
    return base.getChildFile("Martial Systems").getChildFile("LIFT");
}

juce::File SlotStore::slotFile(int slot) const {
    if (!valid(slot)) {
        return {};
    }
    return folder_.getChildFile("slot-" + juce::String(slot).paddedLeft('0', 3) + ".lift");
}

bool SlotStore::exists(int slot) const {
    return valid(slot) && slotFile(slot).existsAsFile();
}

juce::Time SlotStore::modified(int slot) const {
    return exists(slot) ? slotFile(slot).getLastModificationTime() : juce::Time();
}

bool SlotStore::writeAtomic(const juce::File& target, const void* data, size_t size) const {
    if (!folder_.createDirectory()) {
        return false;
    }
    juce::TemporaryFile tmp(target, juce::TemporaryFile::useHiddenFile);
    {
        juce::FileOutputStream out(tmp.getFile());
        if (!out.openedOk() || !out.write(data, size)) {
            return false;
        }
        out.flush();
        if (out.getStatus().failed()) {
            return false;
        }
    }
    return tmp.overwriteTargetFileWithTemporary();
}

bool SlotStore::write(int slot, const juce::MemoryBlock& data) const {
    if (!valid(slot)) {
        return false;
    }
    return writeAtomic(slotFile(slot), data.getData(), data.getSize());
}

bool SlotStore::read(int slot, juce::MemoryBlock& data) const {
    data.reset();
    return exists(slot) && slotFile(slot).loadFileAsData(data) && data.getSize() > 0;
}

int SlotStore::lastSlot() const {
    const int s = folder_.getChildFile("last-slot.txt").loadFileAsString().trim().getIntValue();
    return valid(s) ? s : 0;
}

bool SlotStore::setLastSlot(int slot) const {
    if (!valid(slot)) {
        return false;
    }
    const juce::String s = juce::String(slot) + "\n";
    return writeAtomic(folder_.getChildFile("last-slot.txt"), s.toRawUTF8(), s.getNumBytesAsUTF8());
}

}  // namespace lift
