| SPDX-License-Identifier: MIT
| digisophie-dn pages.s — no page-constructor patches.
|
| SOPHIE borrows stock SYN ranges only (no param:182-184 minted), so no
| knob slot is rerouted and no jsr site is needed on any OS version. This
| file stays an empty translation unit so mod.json's sources list keeps
| its shape if a future mint needs a home here.
| ColdFire V4; assemble with -mcpu=54455, like core-dn1.
        .section .run, "ax"
        .globl  sophie_pages_anchor
sophie_pages_anchor:
        rts
