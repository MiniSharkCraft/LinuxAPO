# Implementation Plan: Configuration tabs (Issue #18)

## Overview

Add multiple open configuration documents to the Qt editor. The GitHub issue is the task tracker; this file records the implementation order and handoff checkpoint.

## Architecture decisions

- Keep the existing single active `ConfigFile` and `FilterTable` as the visible editor state; snapshot each tab's serialized working bytes and saved baseline when switching.
- Use absolute file paths to deduplicate documents and bind Save, Save As, validation, and recent-file actions to the active tab.
- Refuse to close the final tab so the editor never enters an undefined no-document state.

## Task list

1. [x] Add a tab strip and a UI test for the initial document tab.
2. [x] Add open/switch/deduplicate behavior and tests that edits remain isolated across tabs.
3. [ ] Add dirty-tab close decisions and Save As collision handling with UI tests.
4. [ ] Run focused UI/config tests, format checks, review the diff, then rely on hosted CI for the full build and sanitizer gates.

## Risks

- Asynchronous validation/plugin callbacks must not apply stale results to a newly selected tab; increment the existing configuration revision on every switch.
- Save As must not overwrite a different already-open document by accident.

## Verification

- `skyapo-ui-editor` covers tab creation, duplicate-path selection, edit/switch/save isolation, Save As, and dirty close Save/Discard/Cancel.
- Hosted PR checks cover the exact commit, including build/test, sanitizers, and packaging.
