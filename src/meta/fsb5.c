#include "meta.h"
#include "../coding/coding.h"
#include "../util.h"

/* FSB5 - FMOD Studio multiplatform format */
VGMSTREAM * init_vgmstream_fsb5(STREAMFILE *streamFile) {
    VGMSTREAM * vgmstream = NULL;
    off_t StartOffset = 0, NameOffset = 0;
    off_t SampleHeaderStart = 0, ExtraInfoStart = 0;
    size_t SampleHeaderLength, NameTableLength, SampleDataLength, BaseHeaderLength, StreamSize = 0, ExtraInfoSize = 0;

    uint32_t NumSamples = 0, LoopStart = 0, LoopEnd = 0;
    int LoopFlag = 0, ChannelCount = 0, Version, SampleRate = 0, Codec, Flags = 0;
    int TotalSubsongs, TargetSubsong = streamFile->stream_index;
    int i;

    /* check extension, case insensitive */
    if (!check_extensions(streamFile,"fsb"))
        goto fail;

    if (read_32bitBE(0x00,streamFile) != 0x46534235) /* "FSB5" */
        goto fail;

    /* 0x00 is rare (seen in Tales from Space Vita) */
    Version = read_32bitLE(0x04,streamFile);
    if (Version != 0x00 && Version != 0x01) goto fail;

    TotalSubsongs      = read_32bitLE(0x08,streamFile);
    SampleHeaderLength = read_32bitLE(0x0C,streamFile);
    NameTableLength    = read_32bitLE(0x10,streamFile);
    SampleDataLength   = read_32bitLE(0x14,streamFile);
    Codec              = read_32bitLE(0x18,streamFile);
    /* version 0x01 - 0x1c(4): zero,  0x24(16): hash,  0x34(8): unk
     * version 0x00 has an extra field (always 0?) at 0x1c */
    if (Version == 0x01) {
        Flags = read_32bitLE(0x20,streamFile); /* found by tests and assumed to be flags, no games known */
    }
    BaseHeaderLength   = (Version==0x00) ? 0x40 : 0x3C;

    if ((SampleHeaderLength + NameTableLength + SampleDataLength + BaseHeaderLength) != get_streamfile_size(streamFile)) {
        VGM_LOG("FSB5: bad size (%x + %x + %x + %x != %x)\n", SampleHeaderLength, NameTableLength, SampleDataLength, BaseHeaderLength, get_streamfile_size(streamFile));
        goto fail;
    }

    if (TargetSubsong == 0) TargetSubsong = 1;
    if (TargetSubsong > TotalSubsongs || TotalSubsongs <= 0) goto fail;

    SampleHeaderStart = BaseHeaderLength;

    /* find target stream header and data offset, and read all needed values for later use
     *  (reads one by one as the size of a single stream header is variable) */
    for (i = 1; i <= TotalSubsongs; i++) {
        off_t  DataStart = 0;
        size_t StreamHeaderLength = 0;
        uint32_t SampleMode1, SampleMode2; /* maybe one uint64? */


        SampleMode1 = (uint32_t)read_32bitLE(SampleHeaderStart+0x00,streamFile);
        SampleMode2 = (uint32_t)read_32bitLE(SampleHeaderStart+0x04,streamFile);
        StreamHeaderLength += 0x08;

        /* get samples */
        NumSamples  = ((SampleMode2 >> 2) & 0x3FFFFFFF); /* bits2: 31..2 (30) */

        /* get offset inside data section */
        /* up to 0x07FFFFFF * 0x20 = full 32b offset 0xFFFFFFE0 (recheck, after 0x80000000 some calcs may be off?) */
        DataStart   = ((SampleMode2 & 0x03) << 25) | ((SampleMode1 >> 7) & 0x1FFFFFF) << 5; /* bits2: 1..0 (2) | bits1: 31..8 (25) */

        /* get channels */
        switch ((SampleMode1 >> 5) & 0x03) { /* bits1: 7..6 (2) */
            case 0:  ChannelCount = 1; break;
            case 1:  ChannelCount = 2; break;
            case 2:  ChannelCount = 6; break;/* some Dark Souls 2 MPEG; some IMA ADPCM */
            case 3:  ChannelCount = 8; break;/* some IMA ADPCM */
            /* other channels (ex. 4/10/12ch) use 0 here + set extra flags */
            default: /* not possible */
                goto fail;
        }

        /* get sample rate  */
        switch ((SampleMode1 >> 1) & 0x0f) { /* bits1: 5..1 (4) */
            case 0:  SampleRate = 4000;  break;
            case 1:  SampleRate = 8000;  break;
            case 2:  SampleRate = 11000; break;
            case 3:  SampleRate = 11025; break;
            case 4:  SampleRate = 16000; break;
            case 5:  SampleRate = 22050; break;
            case 6:  SampleRate = 24000; break;
            case 7:  SampleRate = 32000; break;
            case 8:  SampleRate = 44100; break;
            case 9:  SampleRate = 48000; break;
            case 10: SampleRate = 96000; break;
            /* other sample rates (ex. 3000/64000/192000) use 0 here + set extra flags */
            default: /* 11-15: rejected (FMOD error) */
                goto fail;
        }

        /* get extra flags */
        if (SampleMode1 & 0x01) { /* bits1: 0 (1) */
            uint32_t ExtraFlag, ExtraFlagStart, ExtraFlagType, ExtraFlagSize, ExtraFlagEnd;

            ExtraFlagStart = SampleHeaderStart+0x08;
            do {
                ExtraFlag = read_32bitLE(ExtraFlagStart,streamFile);
                ExtraFlagType = (ExtraFlag >> 25) & 0x7F; /* bits 32..26 (7) */
                ExtraFlagSize = (ExtraFlag >> 1) & 0xFFFFFF; /* bits 25..1 (24)*/
                ExtraFlagEnd  = (ExtraFlag & 0x01); /* bit 0 (1) */

                switch(ExtraFlagType) {
                    case 0x01:  /* channels */
                        ChannelCount = read_8bit(ExtraFlagStart+0x04,streamFile);
                        break;
                    case 0x02:  /* sample rate */
                        SampleRate = read_32bitLE(ExtraFlagStart+0x04,streamFile);
                        break;
                    case 0x03:  /* loop info */
                        LoopStart = read_32bitLE(ExtraFlagStart+0x04,streamFile);
                        if (ExtraFlagSize > 0x04) /* probably not needed */
                            LoopEnd = read_32bitLE(ExtraFlagStart+0x08,streamFile);

                        /* when start is 0 seems the song repeats with no real looping (ex. Sonic Boom Fire & Ice jingles) */
                        LoopFlag = (LoopStart != 0x00);
                        break;
                    case 0x04:  /* free comment, or maybe SFX info */
                        break;
                  //case 0x05:  /* Unknown (32b) */
                  //    /* found in Tearaway Vita, value 0, first stream only */
                  //    break;
                    case 0x06:  /* XMA seek table */
                        /* no need for it */
                        break;
                    case 0x07:  /* DSP coefs */
                        ExtraInfoStart = ExtraFlagStart + 0x04;
                        break;
                    case 0x09:  /* ATRAC9 config */
                        ExtraInfoStart = ExtraFlagStart + 0x04;
                        ExtraInfoSize = ExtraFlagSize;
                        break;
                    case 0x0a:  /* XWMA config */
                        ExtraInfoStart = ExtraFlagStart + 0x04;
                        break;
                    case 0x0b:  /* Vorbis setup ID and seek table */
                        ExtraInfoStart = ExtraFlagStart + 0x04;
                        /* seek table format:
                         * 0x08: table_size (total_entries = seek_table_size / (4+4)), not counting this value; can be 0
                         * 0x0C: sample number (only some samples are saved in the table)
                         * 0x10: offset within data, pointing to a FSB vorbis block (with the 16b block size header)
                         * (xN entries)
                         */
                        break;
                  //case 0x0d:  /* Unknown (32b) */
                  //    /* found in some XMA2/Vorbis/FADPCM */
                  //    break;
                    default:
                        VGM_LOG("FSB5: unknown extra flag 0x%x at 0x%04x + 0x04 (size 0x%x)\n", ExtraFlagType, ExtraFlagStart, ExtraFlagSize);
                        break;
                }

                ExtraFlagStart += 0x04 + ExtraFlagSize;
                StreamHeaderLength += 0x04 + ExtraFlagSize;
            } while (ExtraFlagEnd != 0x00);
        }

        /* stream found */
        if (i == TargetSubsong) {
            StartOffset = BaseHeaderLength + SampleHeaderLength + NameTableLength + DataStart;

            /* get stream size from next stream or datasize if there is only one */
            if (i == TotalSubsongs) {
                StreamSize = SampleDataLength - DataStart;
            } else {
                uint32_t NextSampleMode  = (uint32_t)read_32bitLE(SampleHeaderStart+StreamHeaderLength+0x00,streamFile);
                StreamSize = (((NextSampleMode >> 7) & 0x00FFFFFF) << 5) - DataStart;
            }

            break;
        }

        /* continue searching */
        SampleHeaderStart += StreamHeaderLength;
    }
    /* target stream not found*/
    if (!StartOffset || !StreamSize) goto fail;

    /* get stream name */
    if (NameTableLength) {
        NameOffset = BaseHeaderLength + SampleHeaderLength + read_32bitLE(BaseHeaderLength + SampleHeaderLength + 0x04*(TargetSubsong-1),streamFile);
    }


    /* build the VGMSTREAM */
    vgmstream = allocate_vgmstream(ChannelCount,LoopFlag);
    if (!vgmstream) goto fail;

    vgmstream->sample_rate = SampleRate;
    vgmstream->num_samples = NumSamples;
    if (LoopFlag) {
        vgmstream->loop_start_sample = LoopStart;
        vgmstream->loop_end_sample = LoopEnd;
    }
    vgmstream->num_streams = TotalSubsongs;
    vgmstream->stream_size = StreamSize;
    vgmstream->meta_type = meta_FSB5;
    if (NameOffset)
        read_string(vgmstream->stream_name,STREAM_NAME_SIZE, NameOffset,streamFile);


    /* parse codec */
    switch (Codec) {
        case 0x00:  /* FMOD_SOUND_FORMAT_NONE */
            goto fail;

        case 0x01:  /* FMOD_SOUND_FORMAT_PCM8  [Anima - Gate of Memories (PC)] */
            vgmstream->coding_type = coding_PCM8_U;
            vgmstream->layout_type = ChannelCount == 1 ? layout_none : layout_interleave;
            vgmstream->interleave_block_size = 0x01;
            break;

        case 0x02:  /* FMOD_SOUND_FORMAT_PCM16  [Shantae Risky's Revenge (PC)] */
            vgmstream->coding_type = (Flags & 0x01) ? coding_PCM16BE : coding_PCM16LE;
            vgmstream->layout_type = ChannelCount == 1 ? layout_none : layout_interleave;
            vgmstream->interleave_block_size = 0x02;
            break;

        case 0x03:  /* FMOD_SOUND_FORMAT_PCM24 */
            VGM_LOG("FSB5: FMOD_SOUND_FORMAT_PCM24 found\n");
            goto fail;

        case 0x04:  /* FMOD_SOUND_FORMAT_PCM32 */
            VGM_LOG("FSB5: FMOD_SOUND_FORMAT_PCM32 found\n");
            goto fail;

        case 0x05:  /* FMOD_SOUND_FORMAT_PCMFLOAT  [Anima: Gate of Memories (PC)] */
            vgmstream->coding_type = coding_PCMFLOAT;
            vgmstream->layout_type = (ChannelCount == 1) ? layout_none : layout_interleave;
            vgmstream->interleave_block_size = 0x04;
            break;

        case 0x06:  /* FMOD_SOUND_FORMAT_GCADPCM  [Sonic Boom: Fire and Ice (3DS)] */
            if (Flags & 0x02) { /* non-interleaved mode */
                vgmstream->coding_type = coding_NGC_DSP;
                vgmstream->layout_type = layout_interleave;
                vgmstream->interleave_block_size = (StreamSize / ChannelCount);
            }
            else {
                vgmstream->coding_type = coding_NGC_DSP_subint;
                vgmstream->layout_type = layout_none;
                vgmstream->interleave_block_size = 0x02;
            }

	        dsp_read_coefs_be(vgmstream,streamFile,ExtraInfoStart,0x2E);
            break;

        case 0x07:  /* FMOD_SOUND_FORMAT_IMAADPCM  [Skylanders] */
            vgmstream->coding_type = (vgmstream->channels > 2) ? coding_FSB_IMA : coding_XBOX_IMA;
            vgmstream->layout_type = layout_none;
            break;

        case 0x08:  /* FMOD_SOUND_FORMAT_VAG  [from fsbankex tests, no known games] */
            vgmstream->coding_type = coding_PSX;
            vgmstream->layout_type = layout_interleave;
            if (Flags & 0x02) { /* non-interleaved mode */
                vgmstream->interleave_block_size = (StreamSize / ChannelCount);
            }
            else {
                vgmstream->interleave_block_size = 0x10;
            }
            break;

        case 0x09:  /* FMOD_SOUND_FORMAT_HEVAG  [Guacamelee (Vita)] */
            vgmstream->coding_type = coding_HEVAG;
            vgmstream->layout_type = layout_interleave;
            vgmstream->interleave_block_size = 0x10;
            break;

#ifdef VGM_USE_FFMPEG
        case 0x0A: {/* FMOD_SOUND_FORMAT_XMA  [Dark Souls 2 (X360)] */
            uint8_t buf[0x100];
            int bytes, block_size, block_count;

            block_size = 0x8000; /* FSB default */
            block_count = StreamSize / block_size + (StreamSize % block_size ? 1 : 0);

            bytes = ffmpeg_make_riff_xma2(buf, 0x100, vgmstream->num_samples, StreamSize, vgmstream->channels, vgmstream->sample_rate, block_count, block_size);
            vgmstream->codec_data = init_ffmpeg_header_offset(streamFile, buf,bytes, StartOffset,StreamSize);
            if ( !vgmstream->codec_data ) goto fail;
            vgmstream->coding_type = coding_FFmpeg;
            vgmstream->layout_type = layout_none;
            break;
        }
#endif

#ifdef VGM_USE_MPEG
        case 0x0B: {/* FMOD_SOUND_FORMAT_MPEG  [Final Fantasy X HD (PS3), Shantae Risky's Revenge (PC)] */
            mpeg_custom_config cfg = {0};

            cfg.fsb_padding = (vgmstream->channels > 2 ? 16 : 4); /* observed default */

            vgmstream->codec_data = init_mpeg_custom(streamFile, StartOffset, &vgmstream->coding_type, vgmstream->channels, MPEG_FSB, &cfg);
            if (!vgmstream->codec_data) goto fail;
            vgmstream->layout_type = layout_none;
            break;
        }
#endif
        case 0x0C:  /* FMOD_SOUND_FORMAT_CELT  [BIT.TRIP Presents Runner2 (PC), Full Bore (PC)] */
            VGM_LOG("FSB5: FMOD_SOUND_FORMAT_CELT found\n");
            goto fail;

#ifdef VGM_USE_ATRAC9
        case 0x0D: {/* FMOD_SOUND_FORMAT_AT9 */
            atrac9_config cfg = {0};

            cfg.channels = vgmstream->channels;
            switch(ExtraInfoSize) {
                case 0x04: /* Little Big Planet 2ch (Vita), Guacamelee (Vita) */
                    cfg.config_data = read_32bitBE(ExtraInfoStart,streamFile);
                    break;
                case 0x08: /* Day of the Tentacle Remastered (Vita) */
                    /* 0x00: superframe size (also in config_data) */
                    cfg.config_data = read_32bitBE(ExtraInfoStart+0x04,streamFile);
                    break;
                //case 0x0c: /* Little Big Planet 6ch (Vita) */
                //    //todo: this is just 0x04 x3, in case of 4ch would be 0x08 --must improve detection
                //    //each stream has its own config_data (but seem to be the same), interleaves 1 super frame per stream
                //    break;
                default:
                    VGM_LOG("FSB5: unknown extra info size 0x%x\n", ExtraInfoSize);
                    goto fail;
            }
            //cfg.encoder_delay = 0x100; //todo not used? num_samples seems to count all data

            vgmstream->codec_data = init_atrac9(&cfg);
            if (!vgmstream->codec_data) goto fail;
            vgmstream->coding_type = coding_ATRAC9;
            vgmstream->layout_type = layout_none;
            break;
        }
#endif

#ifdef VGM_USE_FFMPEG
        case 0x0E: { /* FMOD_SOUND_FORMAT_XWMA  [from fsbankex tests, no known games] */
            uint8_t buf[0x100];
            int bytes, format, average_bps, block_align;

            format = read_16bitBE(ExtraInfoStart+0x00,streamFile);
            block_align = (uint16_t)read_16bitBE(ExtraInfoStart+0x02,streamFile);
            average_bps = (uint32_t)read_32bitBE(ExtraInfoStart+0x04,streamFile);
            /* rest: seek entries + mini seek table? */
            /* XWMA encoder only does up to 6ch (doesn't use FSB multistreams for more) */

            bytes = ffmpeg_make_riff_xwma(buf,0x100, format, StreamSize, vgmstream->channels, vgmstream->sample_rate, average_bps, block_align);
            vgmstream->codec_data = init_ffmpeg_header_offset(streamFile, buf,bytes, StartOffset,StreamSize);
            if ( !vgmstream->codec_data ) goto fail;
            vgmstream->coding_type = coding_FFmpeg;
            vgmstream->layout_type = layout_none;
            break;
        }
#endif

#ifdef VGM_USE_VORBIS
        case 0x0F: {/* FMOD_SOUND_FORMAT_VORBIS  [Shantae Half Genie Hero (PC), Pokemon Go (iOS)] */
            vorbis_custom_config cfg = {0};

            cfg.channels = vgmstream->channels;
            cfg.sample_rate = vgmstream->sample_rate;
            cfg.setup_id = read_32bitLE(ExtraInfoStart,streamFile);

            vgmstream->layout_type = layout_none;
            vgmstream->coding_type = coding_VORBIS_custom;
            vgmstream->codec_data = init_vorbis_custom(streamFile, StartOffset, VORBIS_FSB, &cfg);
            if (!vgmstream->codec_data) goto fail;

            break;
        }
#endif

        case 0x10:  /* FMOD_SOUND_FORMAT_FADPCM  [Dead Rising 4 (PC), Sine Mora Ex (Switch)] */
            vgmstream->coding_type = coding_FADPCM;
            vgmstream->layout_type = layout_interleave;
            vgmstream->interleave_block_size = 0x8c;
            break;

        default:
            VGM_LOG("FSB5: unknown codec %x found\n", Codec);
            goto fail;
    }

    if (!vgmstream_open_stream(vgmstream,streamFile,StartOffset))
        goto fail;

    return vgmstream;

fail:
    close_vgmstream(vgmstream);
    return NULL;
}
