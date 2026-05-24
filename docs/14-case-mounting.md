# 14 — Mount the case and run the display ribbon to the dash

## Goal

Physically install the controller under the dash and run the 5"
display to a visible spot on the dash, using only **removable**
fasteners. No drilled holes, no cut harness, no permanent
modifications. A future owner with a scan tool should be able to undo
this install in 10 minutes and have no evidence it was ever there.

## Prerequisites

- [13 — OBD-II install](13-obd2-install.md) wiring complete, powered-up
  bench check passed
- Hardware from the [BOM](BOM.md):
  - Hammond 1591ESBK ABS enclosure (150×80×50 mm) — or the smaller
    1591BSBK if your perfboard fits, same family
  - 3M VHB tape strip (BOM row 23)
  - Heavy-duty Velcro / hook-and-loop strips (One-Wrap or similar)
  - Removable wire ties (releasable / reusable type — *not* the
    one-shot ratcheting kind)
  - 433 MHz SMA whip antenna (BOM row 5)
  - 2.4 GHz SMA antenna for ESP32 WiFi (BOM row 6)
  - Mini-HDMI to HDMI 50 cm flat ribbon (BOM row 11)
  - USB micro-to-A short cable for the touch controller (BOM row 12)
- Gloves and a flashlight. Most of this happens upside down with the
  driver's-side under-dash panel removed.

## Step 1 — Pre-install dry-fit on the desk

Before you crawl under the dash, lay everything out on a bench in the
final arrangement:

- Case, lid open, perfboard mounted inside
- OBD-II pigtail exiting through the grommet (BOM row 22)
- Both antennas mounted to the case (drill the SMA bulkhead holes
  now if you haven't already; step drill, 6.5 mm)
- 50 cm HDMI ribbon coiled, ready to leave the case through the same
  grommet or a second one
- USB cable for the touch controller bundled with the HDMI ribbon

Power it up on the bench one last time, confirm the dashboard renders
on the 5" display, the dashboard buttons work, and the ESP32 is on
WiFi. Easier to fix something now than under the dash.

## Step 2 — Case mounting location

Target: behind the lower driver's-side knee panel, above the OBD-II
port. On the Spectra this panel pops off with three plastic clips
and one 10 mm bolt; have the panel off and a flashlight ready.

Criteria for the mounting spot:

- **Off the floor** by at least 10 cm. Water from wet shoes pools
  along the carpet edge in winter; you do not want the case sitting
  in it.
- **Away from the steering column** — the column has airbag wiring
  and a clockspring you absolutely do not touch.
- **Away from the HVAC ducts** — they get hot enough in summer to
  push the Pi Zero 2 W past its 70 °C `cpu_thermal` throttling
  point. Hand-warm in your hand is fine; "uncomfortable" is too
  hot.
- **Close to the OBD-II splitter**, ideally within 30 cm of pigtail
  cable so you don't have a coil of unmanaged wire flopping around.

On the Spectra a good spot is the back of the lower steering-column
trim, just above the OBD-II socket. The case sits flat against the
plastic with the lid facing rearward (away from the firewall).

## Step 3 — Attach the case with removable fasteners

**Velcro for the case body, removable wire ties for cable anchoring.**

- Cut two strips of heavy-duty Velcro hook to fit the long edges of
  the case lid (bottom face that contacts the dash plastic). Mate
  with corresponding loop strips on the dash plastic.
- Confirm the case is held firmly by hand-tugging in all four
  directions before letting go. Velcro that holds a slim case full
  of breakouts holds fine; verify anyway.
- Use 2-3 removable wire ties to anchor the OBD pigtail cable to the
  existing factory harness — never the steering column wiring, but
  any of the body-side harness tie-points are fair game.
- **Do not** use 3M VHB tape on the case body. VHB removes paint
  and plastic-skin texture; reserve it for the display dash mount
  in step 5.

If you absolutely need a more secure mount than Velcro, use the same
removable wire ties to lash the case to a factory bracket. The build
must remain removable.

## Step 4 — Antenna placement

The 433 MHz transmit needs line-of-sight (or close to it) to the
Compustar brain antenna. On the Spectra the brain is behind the
glovebox; the brain's antenna whip routes up behind the A-pillar trim
on the passenger side.

| Antenna | Length | Placement |
|---|---|---|
| 433 MHz quarter-wave whip | ~17 cm | Out the top of the case, vertical, **not** behind metal trim. Best spot: along the side of the lower knee panel, tip pointing up toward the windshield. |
| 2.4 GHz WiFi | ~7 cm | Wherever fits; 2.4 GHz couples fine through most plastic dash trim. Avoid pressing it flat against bare sheet metal. |

The 433 MHz whip is the one that matters. If your in-car Lock test
fails at range, this is the first thing to revisit — try moving the
antenna closer to the windshield or up over the dash top. A few cm
of repositioning often makes the difference between "fails 50% of
the time" and "100% reliable".

**Do not** mount either antenna behind the metal-foil-backed
insulation that the Spectra has under the dash (between the dash
plastic and the firewall). It will attenuate 433 MHz by 20+ dB.

## Step 5 — 5" HDMI display routing

Display target location: top center of the dash, just above the
factory radio bezel. The Spectra has a small flat shelf there
(originally designed for a slide-in stereo head). The display fits
with about 5 mm of clearance on all sides.

Cable routing from the case (under-dash) to the display (top of
dash):

1. Route the HDMI ribbon (mini-HDMI end at the Pi, full-size HDMI
   at the display) up the right side of the steering column,
   tucked behind the factory dash seam.
2. Route the USB cable for the touch controller bundled with the
   HDMI ribbon — they share the same path.
3. At the top of the dash, both cables come up through the existing
   gap between the dash top and the windshield base. Don't drill a
   new hole; the gap is there because the dash floats free of the
   pinch weld.
4. Slack-loop the cables behind the display so you can pop it off
   without unplugging anything.

Mount the display itself with a single strip of 3M VHB tape on the
back. **This is the only place VHB is appropriate** in the install —
the dash plastic is a low-energy surface and VHB is the only
removable adhesive that holds reliably on it. Yes, removing the
display will pull a small patch of the dash texture; that's an
accepted cost for "rock solid mount" in the design tradeoff.

If you want zero-residue removal, substitute a Brodit / ProClip
vehicle-specific mount instead — those clip into existing dash seams
without adhesive. Brodit makes a Spectra-2009 mount; about $40 CAD.

## Step 6 — Heat considerations

The under-dash area on the Spectra hits ~50 °C in summer with the
sun on the windshield, doors closed. The Pi Zero 2 W's official
operating range is 0-50 °C ambient; CPU throttling kicks in around
70 °C die temperature, which is comfortably above ambient in our
case.

- Leave the case **vented** — drill 4-6 small (3 mm) vent holes on
  the lid and bottom face. Don't bother with a fan; the natural
  convection is enough at our duty cycle.
- The ESP32 itself doesn't care about heat at our load levels; it's
  rated 105 °C die.
- The buck converter is the part to watch. The buck specified in
  the BOM has its own thermal shutdown at ~125 °C; if it triggers,
  the 5 V rail just drops out and the ESP32 resets cleanly. Not
  great, but not catastrophic.
- Track this with the ESP32's internal temperature sensor (publishes
  to MQTT as part of the STATUS message). If you see sustained
  >55 °C ambient inside the case during a heatwave, revisit the
  mounting location.

## Step 7 — Waterproofing

Don't.

This is an interior install in a sealed cabin. The Hammond enclosure
is rated IP54 out of the box, which is overkill for under-dash.
Vent holes from step 6 are more important than a watertight seal.

If you ever extend this build with an engine-bay component (e.g. a
hood-pin sensor), **that** part needs proper IP67 enclosure and
gasketed entry — that's a separate enclosure, not this case.

## Step 8 — Removability test

Before you put the under-dash panel back on, do a removal dry-run:

1. Pop the case off its Velcro
2. Unplug the OBD-II pigtail at the splitter
3. Unplug the splitter from the car
4. Pop the display off its VHB mount (lift one corner, peel slowly)
5. Pull the HDMI + USB cable bundle back down through the dash gap
6. Confirm: zero residue except a small patch of dash texture
   pulled by the display VHB

Total disassembly time: should be under 10 minutes. If it's longer,
you've over-secured something — back off on the anchoring.

Re-install everything for the next step.

## What you should have when done

- Case Velcro-mounted under the driver's-side dash, above the OBD
  socket
- Both antennas routed for good RF — 433 MHz vertical and clear of
  metal
- 5" display VHB-mounted to the top center of the dash
- HDMI + USB cables run behind the steering column trim with slack
  loops at both ends
- Vent holes drilled in the case
- Pigtail cable strain-relieved with removable wire ties
- Full removability confirmed by dry-run

## Where artifacts go

- Photos of the as-mounted case + display to
  `logs/images/YYYY-MM-DD/install-*.jpg`
- Any deviation from the recommended mounting location (different
  trim car, etc.) noted in that day's `logs/YYYY-MM-DD.md`

## Troubleshooting

| Symptom | Fix |
|---|---|
| Case rattles over bumps | Velcro patch is too small or wrong type. Use heavy-duty One-Wrap style. |
| Display tilts forward when you tap it | VHB needs 24 h to fully cure. Tape only the corners initially, add more after the first day. |
| HDMI ribbon kinks at the column | The 50 cm flat cable from the BOM has a minimum bend radius — try a 1 m cable with a gentler curve. |
| 433 MHz transmits fine on the bench but doesn't reach the brain in the car | Antenna behind metal-foil insulation, or pointing into the firewall. Reposition vertical, tip up. |
| Pi throttles in summer | Add vent holes (step 6) or relocate the case away from HVAC ducting. |

## Safety callouts

- **Disconnect the OBD pigtail** from the splitter before touching
  any wiring under the dash. The 12 V rail is always hot; even with
  the key out, a wrench across pin 16 to ground welds itself.
- **Do not** route any cables near the steering column airbag
  clockspring. If you can see a yellow connector, you're too close.
- **Do not** mount anything in front of the airbag deployment zone
  in the dash top. The display location described above (above the
  factory radio bezel) is well clear of the airbag — verify the
  same is true on whatever car you adapt this to.

## Next

[15 — First live trigger (manual, parked, hood open)](15-first-trigger.md)
— first time you fire the engine from the in-car install, with all
the safety guards in place.
