#pragma once

#include <string>

// Cook step (docs/ROADMAP.md stage 4 "Asset cooking + pack files"): `<game>.exe --cook <out.pak>` from a development build.
// build.bat Export runs it before gathering the shipping folder, so the shipping exe gets one pack instead of the assets folder.
//
// What goes in: the start scene, the project's `scenes` and `packInclude` (files or folders), and every data file the engine opens by
// fixed name or folder (tables, tuning, input bindings, sound events, feedback, effects, UI, strings, fonts). From there references are
// followed: any word in a text asset that is the path of an existing asset (scene → models, prefabs, terrain, HDRI, probes; sounds.events
// → clips; .ui → images; cutscenes.table → camera glTF). Only what's reached is packed; there's no "whole folder" like Unity's Resources.
// String literals in the game's source code that name an existing asset the cook didn't reach are warned about (add them to packInclude).
//
// How: glTF → .mkmodel (+ .mkcam for cameras), .hdr → .mkenv (asset/Cooked.h), the rest as is, all LZ4 in the pack (platform/Pack.h).
// mealkit.project goes in too (without the engine/engineVersion lines that hold dev PC paths). Next to the pack, <out.pak>.txt has the
// content hash and the file list (MealKitExport.cmake writes the hash into build.txt).
namespace mealkit::game {

// false if the project can't be read or the pack can't be written (missing referenced files are warnings). Only in builds with
// MEALKIT_ASSET_IMPORT (dev builds); elsewhere it returns false with a message
bool cookGame(const std::string& outPack);

} // namespace mealkit::game
