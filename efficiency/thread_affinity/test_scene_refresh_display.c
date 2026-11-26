// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include "scene_refresh_display.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static FILE *fixture(unsigned count, bool duplicates, bool incomplete, bool no_active)
{
	FILE *file = tmpfile();
	assert(file);
	fputs("irrelevant initial display diagnostics\nPacesetter Display 4630947185118785939\n", file);
	if (!no_active)
		fputs(" activeMode={id=0, hwcId=0, resolution=1272x2800, vsyncRate=120.00 Hz}\n", file);
	fputs(" displayModes=\n", file);
	for (unsigned i = 0; i < count; i++)
		fprintf(file, "  {id=%u, hwcId=%u, resolution=1272x2800, vsyncRate=%u.00 Hz}\n",
			i, i, duplicates ? 120 : 120 + i);
	fputs(" {id=1000, hwcId=1000, resolution=1080x2378, vsyncRate=120.00 Hz}\n", file);
	if (!incomplete) fputs(" displayManagerPolicy={defaultModeId=0}\n", file);
	rewind(file);
	return file;
}

static void invalid(const char *text)
{
	FILE *file = fmemopen((void *)text, strlen(text), "r");
	struct srd_snapshot out = { .active_mode_id = 77 };
	assert(file);
	assert(srd_parse(file, &out) == -1);
	assert(errno == EPROTO);
	assert(out.active_mode_id == 77 && !out.modes && out.count == 0);
	fclose(file);
}

int main(void)
{
	/* Dynamic growth and resolution filtering; mode IDs are not frequency. */
	FILE *file = fixture(80, false, false, false);
	struct srd_snapshot out = {0};
	assert(srd_parse(file, &out) == 0);
	assert(out.count == 80 && out.active_mode_id == 0);
	assert(out.physical_display_id == UINT64_C(4630947185118785939));
	assert(out.modes[0].mode_id == 0 && out.modes[0].hz == 120);
	assert(out.modes[79].mode_id == 79 && out.modes[79].hz == 199);
	fclose(file);
	file = fixture(9, false, false, false);
	/* Reuse a previously returned object; ASan/LSan catches lost old modes. */
	assert(srd_parse(file, &out) == 0 && out.count == 9);
	srd_snapshot_free(&out);
	assert(!out.modes && !out.count);
	fclose(file);
	for (unsigned i = 0; i < 3; i++) {
		file = fixture(i == 0 ? 2 : 1, i == 0, i == 1, i == 2);
		assert(srd_parse(file, &out) == -1 && errno == EPROTO);
		fclose(file);
	}
	invalid("Pacesetter Display 1\n activeMode={id=1, hwcId=0, resolution=1x1, vsyncRate=60.00 Hz}\n displayModes=\n {id=0, hwcId=0, resolution=1x1, vsyncRate=60.00 Hz}\n displayManagerPolicy={}\n");
	invalid("Pacesetter Display 1\n activeMode={id=0, hwcId=0, resolution=1x1, vsyncRate=60.00 Hz}\n displayModes=\n {id=0, hwcId=0, resolution=1x1, vsyncRate=nan Hz}\n displayManagerPolicy={}\n");
	invalid("Pacesetter Display 1\n activeMode={id=0, hwcId=0, resolution=1x1, vsyncRate=60.00 Hz}\n displayModes=\n {id=0, hwcId=0, resolution=1x1, vsyncRate=60.00 Hz}\n {id=0, hwcId=0, resolution=2x2, vsyncRate=90.00 Hz}\n displayManagerPolicy={}\n");
	invalid("Pacesetter Display 1\nPacesetter Display 2\n activeMode={id=0, hwcId=0, resolution=1x1, vsyncRate=60.00 Hz}\n displayModes=\n {id=0, hwcId=0, resolution=1x1, vsyncRate=60.00 Hz}\n displayManagerPolicy={}\n");
	puts("Scene display catalog: 9 parser cases passed");
	return 0;
}
