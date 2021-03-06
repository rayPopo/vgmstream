#include "meta.h"
#include "../util.h"


/* SWAV - wave files generated by the DS SDK */
VGMSTREAM* init_vgmstream_swav(STREAMFILE* sf) {
    VGMSTREAM* vgmstream = NULL;
    int channel_count, loop_flag;
    off_t start_offset;
    int codec_number, bits_per_sample;
    coding_t coding_type;


    /* checks */
    /* .swav: standard
     * .adpcm: Merlin - A Servant of Two Masters (DS) */
    if (!check_extensions(sf, "swav,adpcm"))
        goto fail;

    if (read_u32be(0x00,sf) != 0x53574156) /* "SWAV" */
        goto fail;
    if (read_u32be(0x10,sf) != 0x44415441) /* "DATA" */
        goto fail;

    /* check type details */
    codec_number = read_8bit(0x18,sf);
    loop_flag = read_8bit(0x19,sf);

    channel_count = 1;
    if (get_streamfile_size(sf) != read_s32le(0x08,sf)) {
        if (get_streamfile_size(sf) != (read_s32le(0x08,sf) - 0x24) * 2 + 0x24)
            goto fail;
        channel_count = 2;
    }

    switch (codec_number) {
        case 0:
            coding_type = coding_PCM8;
            bits_per_sample = 8;
            break;
        case 1:
            coding_type = coding_PCM16LE;
            bits_per_sample = 16;
            break;
        case 2:
            coding_type = coding_IMA_int;
            bits_per_sample = 4;
            break;
        default:
            goto fail;
    }
    start_offset = 0x24;


    /* build the VGMSTREAM */
    vgmstream = allocate_vgmstream(channel_count, loop_flag);
    if (!vgmstream) goto fail;

    vgmstream->num_samples = (read_s32le(0x14,sf) - 0x14) * 8 / bits_per_sample;
    vgmstream->sample_rate = read_u16le(0x1A,sf);
    if (loop_flag) {
        vgmstream->loop_start_sample = read_u16le(0x1E,sf) * 32 / bits_per_sample;
        vgmstream->loop_end_sample = read_s32le(0x20,sf) * 32 / bits_per_sample + vgmstream->loop_start_sample;
    }

    if (coding_type == coding_IMA_int) {
        /* handle IMA frame header */
        vgmstream->loop_start_sample -= 32 / bits_per_sample;
        vgmstream->loop_end_sample -= 32 / bits_per_sample;
        vgmstream->num_samples -= 32 / bits_per_sample;

        {
            int i;
            for (i = 0; i < channel_count; i++) {
                vgmstream->ch[i].adpcm_history1_32 = read_s16le(start_offset + 0 + 4*i, sf);
                vgmstream->ch[i].adpcm_step_index = read_s16le(start_offset + 2 + 4*i, sf);
            }
        }

        start_offset += 4 * channel_count;
    }

    vgmstream->coding_type = coding_type;
    vgmstream->meta_type = meta_SWAV;
    if (channel_count == 2) {
        vgmstream->layout_type = layout_interleave;
        vgmstream->interleave_block_size = 1;
    } else {
        vgmstream->layout_type = layout_none;
    }

    if (!vgmstream_open_stream(vgmstream, sf, start_offset))
        goto fail;
    return vgmstream;

fail:
    close_vgmstream(vgmstream);
    return NULL;
}
