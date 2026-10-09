#pragma once
// LittleFS mount helper.
//
// Both the config loader and the token store need the filesystem, and each used
// to run its own begin()/format() dance. Mounting lazily but only once keeps a
// single source of truth and avoids a second format-on-failure pass.
bool storageMount();
