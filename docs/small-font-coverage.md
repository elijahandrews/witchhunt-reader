# Small HD font coverage

The shared scaled-glyph sampler now rounds exact halfway coverage values to the
nearest even level. HD font rasters already contain four quantized levels; reducing
them creates many exact ties. Always rounding those ties upward added gray fringe
and could make small stems look too heavy. Non-tie samples, spacing, baselines,
source font data, and darkness mappings are unchanged. No font files need replacing.

At Bookerly HD 10 pt with Normal darkness, the regular capital T kept its 46 solid
black pixels. Its nonwhite area changed from 104 to 76 pixels; weighted logical ink
changed from 71.33 to 62.0 pixel-equivalents, compared with 62.61 for an area average
of the unquantized larger raster. These are renderer measurements, not calibrated
physical screen reflectance.

The independent comparison covers 24 four-face font files across three families
and eight sizes, with 9,120 ASCII glyphs. Aggregate coverage bias versus the
unquantized area reference changed from +1.96% through +6.13% to -1.71% through
-0.48%. Some individual pixel errors improve and some worsen; four-level
quantization remains lossy. Nearest-even reduces directional tie bias and does
not guarantee zero bias for every glyph or font.

Production renderer tests include literal quarter-level samples with independently
specified expected output, all darkness levels, cache and direct paths, native-size
capitals and lowercase stems, combining marks, and thin-stroke safeguards. The new
tie tests fail with the previous rounding.

For a device check, select Bookerly HD, 10 pt, Normal darkness, and antialiasing.
Open the synthetic test EPUB's Small font strokes chapter and inspect the settled
page. Compare T/t, H/h, N/n, I/i, and L/l across styles. The initial black-and-white
page still precedes the grayscale overlay; this correction does not combine the
physical refreshes or eliminate their visible transition.
