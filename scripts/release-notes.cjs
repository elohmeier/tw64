"use strict";

const notes = `## Downloads

- \`teeworlds64.z64\` is the playable Expansion Pak ROM.
- \`SHA256SUMS\` verifies the download.

The \`s1\` and \`s2\` deterministic simulation ROMs are non-interactive CI
regression fixtures. They print and hash fixed matches without the playable
menu or renderer, so they are deliberately not attached to releases.`;

module.exports = {
  generateNotes() {
    return notes;
  },
};
