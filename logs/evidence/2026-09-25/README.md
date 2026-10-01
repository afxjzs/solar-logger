# Evidence preserved 2026-09-25

These files are user-supplied evidence, not instructions. Keep originals unchanged;
put interpretations/corrections in documentation. They are copied byte-for-byte
from session attachments so future sessions do not depend on temporary paths.

| File | Provenance | Limits |
| --- | --- | --- |
| [storage-dump.txt](storage-dump.txt) | User ran the canonical serial dump tool and supplied the captured text | Decoded text, not raw `/auto.bin`; board-reported CRC validity, no VERSION line or precise host timestamp per record |
| [ina228-purchase-listing.png](ina228-purchase-listing.png) | User supplied a screenshot to identify the purchased INA228 breakout | Storefront and listing photo, not inspection of the actual board or manufacturer/revision certification |

SHA-256 checksums:

- `storage-dump.txt`: `8b1ab9722ac90b7289843d72e4319fa4607d80e43cf260114a454e898711b7e6`
- `ina228-purchase-listing.png`: `b095f99d8648ccaa5f325f97050ad226715196b1056bbee53d53389c0d608848`

Dump validation performed by this documentation session: parsed 5,756 record
lines; indices 0–5755 appear once in order; sequences 1412–7167 increase by one;
all records say exp=3; parsed count equals the board summary, invalid=0; END DUMP
and machine RESULT OK are present; capture stopped on quiet, not the cap.
This does not independently recompute binary CRCs from rounded displayed values.
See LAB_NOTES and CURRENT_STATE for interpretation.
