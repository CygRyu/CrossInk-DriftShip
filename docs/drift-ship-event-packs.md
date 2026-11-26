# Drift Ship event packs

Every Drift Ship session writes one line in the captain's log, shown on the
sleep screen above the list of what happened:

> Day 47. Sensors swept a drowned observatory and found a sealed letter. A rival
> scavenger got there first. Mara Lind wanted to go back for more.

The line is put together from parts: a **place**, a **finding**, a **twist** (on
sessions with a good or bad twist) and, most of the time, a **crew reaction**.
The firmware has a built-in set of parts. Event packs add more without
reflashing.

Event packs only change the story text. They can't change Fuel, finds, crew or
any other part of the game.

## Installing a pack

1. Create the folder `/driftship/events/` on the SD card.
2. Copy one or more `.txt` pack files into it.
3. Read as usual. New parts show up in the log from the next session on.

The firmware reads up to 8 pack files per session (the first 8 the card lists).
Files that don't end in `.txt` are ignored.

## Writing a pack

A pack is a plain UTF-8 text file. Lines starting with `#` are comments. A line
in square brackets starts a section, and every following line is one part for
that section, until the next section.

```text
# My first Drift Ship event pack

[place]
a moon covered in old radio dishes
the wreck of a pleasure yacht

[place ruin-belt]
a library carved into an asteroid

[finding]
a jar of preserved honey
a recording of a choir

[good]
The autopilot found a calm lane through the storm.

[bad]
A swarm of ice grit sanded the sensor dome.

[reaction]
laughed for the first time in weeks

[reaction night-owl]
named the new star after an old friend
```

### Sections

| Section | What it holds | How it is used |
| --- | --- | --- |
| `[place]` | A place, with its article: `an ice moon` | "Sensors swept **an ice moon** and found ..." |
| `[place <era>]` | A place that only appears in one galaxy era | Same, but only in that era |
| `[finding]` | Something found, with its article: `a music box` | "... and found **a music box**." |
| `[good]` | A full sentence for a good twist | Added after the first sentence |
| `[bad]` | A full sentence for a bad twist | Added after the first sentence |
| `[reaction]` | What a crew member did, starting with a verb: `just smiled` | "Mara Lind **just smiled**." |
| `[reaction <trait>]` | A reaction only for crew with that trait | Same, for that trait |

Era names: `outer-rim`, `ruin-belt`, `dense-core`, `deep-void`,
`shattered-arm`, `far-galaxy`.

Trait names: `steady-hands`, `night-owl`, `hoarder`, `lucky`, `reckless`,
`loner`, `dreamer`.

A section the firmware doesn't know (for example, from a pack written for a
newer version) is skipped along with its lines.

### Limits and tips

- Keep parts short. A part is cut at 95 bytes.
- Only the first 32 KB of each pack file is read, which is plenty for a few hundred parts.
- Write places and findings in lower case, so they read well in the middle of
  a sentence.
- The firmware avoids repeating a part it used in the last few sessions, so
  bigger packs repeat less.
- Pack text is shown as written. Placeholders such as `{crew}` are not expanded
  inside pack parts.

## Translating the built-in parts

The built-in parts and the sentence templates are translation strings
(`STR_DRIFT_SHIP_LOG_*`, `STR_DRIFT_SHIP_PLACE_*`, `STR_DRIFT_SHIP_FINDING_*`,
`STR_DRIFT_SHIP_GOOD_*`, `STR_DRIFT_SHIP_BAD_*`, `STR_DRIFT_SHIP_REACTION_*` in
`lib/I18n/translations/`). In the templates, keep the `{day}`, `{place}`,
`{finding}`, `{crew}` and `{reaction}` placeholders, braces included. They can
move anywhere in the sentence.

## Files

| Path | What |
| --- | --- |
| `/driftship/events/*.txt` | Event packs (yours) |
| `/.crosspoint/driftship_story.bin` | The latest log line and which parts were used recently. Deleted by Reset Drift Ship. |
