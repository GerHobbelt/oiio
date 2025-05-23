// Copyright Contributors to the OpenImageIO project.
// SPDX-License-Identifier: Apache-2.0
// https://github.com/AcademySoftwareFoundation/OpenImageIO

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <OpenImageIO/dassert.h>
#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/imagebufalgo_util.h>
#include <OpenImageIO/imageio.h>

#include "rla_pvt.h"

OIIO_PLUGIN_NAMESPACE_BEGIN

using namespace RLA_pvt;


class RLAInput final : public ImageInput {
public:
    RLAInput() { init(); }
    ~RLAInput() override { close(); }
    const char* format_name(void) const override { return "rla"; }
    int supports(string_view feature) const override
    {
        return feature == "ioproxy";
    }
    bool open(const std::string& name, ImageSpec& newspec) override;
    bool open(const std::string& name, ImageSpec& newspec,
              const ImageSpec& config) override;
    int current_subimage(void) const override
    {
        lock_guard lock(*this);
        return m_subimage;
    }
    bool seek_subimage(int subimage, int miplevel) override;
    bool close() override;
    bool read_native_scanline(int subimage, int miplevel, int y, int z,
                              void* data) override;

private:
    std::string m_filename;            ///< Stash the filename
    RLAHeader m_rla;                   ///< Wavefront RLA header
    std::vector<unsigned char> m_buf;  ///< Buffer the image pixels
    int m_subimage;                    ///< Current subimage index
    std::vector<uint32_t> m_sot;       ///< Scanline offsets table
    int m_stride;                      ///< Number of bytes a contig pixel takes

    /// Reset everything to initial state
    ///
    void init()
    {
        ioproxy_clear();
        m_buf.clear();
    }

    /// Helper: read buf[0..nitems-1], swap endianness if necessary
    template<typename T> bool read(T* buf, size_t nitems = 1)
    {
        if (!ioread(buf, sizeof(T), nitems))
            return false;
        if (littleendian()
            && (std::is_same<T, uint16_t>::value
                || std::is_same<T, int16_t>::value
                || std::is_same<T, uint32_t>::value
                || std::is_same<T, int32_t>::value)) {
            swap_endian(buf, nitems);
        }
        return true;
    }

    /// Helper function: translate 3-letter month abbreviation to number.
    ///
    inline int get_month_number(string_view);

    /// Helper: read the RLA header and scanline offset table.
    ///
    inline bool read_header();

    /// Helper: read and decode a single channel group consisting of
    /// channels [first_channel .. first_channel+num_channels-1], which
    /// all share the same number of significant bits.
    bool decode_channel_group(int first_channel, short num_channels,
                              short num_bits, int y);

    /// Helper: decode a span of n RLE-encoded bytes from encoded[0..elen-1]
    /// into buf[0],buf[stride],buf[2*stride]...buf[(n-1)*stride].
    /// Return the number of encoded bytes we ate to fill buf.
    size_t decode_rle_span(unsigned char* buf, int n, int stride,
                           const char* encoded, size_t elen);

    /// Helper: determine channel TypeDesc
    inline TypeDesc get_channel_typedesc(short chan_type, short chan_bits);

    // debugging aid
    void preview(std::ostream& out)
    {
        auto pos = iotell();
        Strutil::print(out, "@{}, next 4 bytes are ", pos);
        union {  // trickery to avoid punned pointer warnings
            unsigned char c[4];
            uint16_t s[2];
            uint32_t i;
        } u;
        ioread((char*)&u.c, 4);  // because it's char, it didn't swap endian
        uint16_t s[2] = { u.s[0], u.s[1] };
        uint32_t i    = u.i;
        if (littleendian()) {
            swap_endian(s, 2);
            swap_endian(&i);
        }
        Strutil::print(out,
              "{:d}/{:d} {:d}/{:d} {:d}/{:d} {:d}/{:d} ({:d} {:d}) ({:d})\n",
              u.c[0], ((char*)u.c)[0], u.c[1], ((char*)u.c)[1], u.c[2],
              ((char*)u.c)[2], u.c[3], ((char*)u.c)[3], s[0], s[1], i);
        ioseek(pos);
    }
};



// Obligatory material to make this a recognizable imageio plugin:
OIIO_PLUGIN_EXPORTS_BEGIN

OIIO_EXPORT ImageInput*
rla_input_imageio_create()
{
    return new RLAInput;
}

OIIO_EXPORT int rla_imageio_version = OIIO_PLUGIN_VERSION;

OIIO_EXPORT const char*
rla_imageio_library_version()
{
    return nullptr;
}

OIIO_EXPORT const char* rla_input_extensions[] = { "rla", nullptr };

OIIO_PLUGIN_EXPORTS_END



bool
RLAInput::open(const std::string& name, ImageSpec& newspec,
               const ImageSpec& config)
{
    // Check 'config' for any special requests
    ioproxy_retrieve_from_config(config);
    return open(name, newspec);
}



bool
RLAInput::open(const std::string& name, ImageSpec& newspec)
{
    m_filename = name;

    if (!ioproxy_use_or_open(name))
        return false;
    ioseek(0);

    // set a bogus subimage index so that seek_subimage actually seeks
    m_subimage = 1;

    bool ok = seek_subimage(0, 0);
    newspec = spec();
    return ok;
}



inline bool
RLAInput::read_header()
{
    // Read the image header, which should have the same exact layout as
    // the m_rla structure (except for endianness issues).
    static_assert(sizeof(m_rla) == 740, "Bad RLA struct size");
    if (!read(&m_rla)) {
        errorfmt("RLA could not read the image header");
        return false;
    }
    m_rla.rla_swap_endian();  // fix endianness

    if (m_rla.Revision != (int16_t)0xFFFE
        && m_rla.Revision != 0 /* for some reason, this can happen */) {
        errorfmt("RLA header Revision number unrecognized: {}", m_rla.Revision);
        return false;  // unknown file revision
    }
    if (m_rla.NumOfChannelBits < 0 || m_rla.NumOfChannelBits > 32
        || m_rla.NumOfMatteBits < 0 || m_rla.NumOfMatteBits > 32
        || m_rla.NumOfAuxBits < 0 || m_rla.NumOfAuxBits > 32) {
        errorfmt("Unsupported bit depth, or maybe corrupted file.");
        return false;
    }
    if (m_rla.NumOfChannelBits == 0)
        m_rla.NumOfChannelBits = 8;  // apparently, this can happen

    // Immediately following the header is the scanline offset table --
    // one uint32_t for each scanline, giving absolute offsets (from the
    // beginning of the file) where the RLE records start for each
    // scanline of this subimage.
    m_sot.resize(std::abs(m_rla.ActiveBottom - m_rla.ActiveTop) + 1, 0);
    if (!read(&m_sot[0], m_sot.size())) {
        errorfmt("RLA could not read the scanline offset table");
        return false;
    }
    return true;
}



bool
RLAInput::seek_subimage(int subimage, int miplevel)
{
    if (miplevel != 0 || subimage < 0)
        return false;

    if (subimage == current_subimage())
        return true;  // already on the right level

    // RLA images allow multiple subimages; they are simply concatenated
    // together, with image N's header field NextOffset giving the
    // absolute offset of the start of image N+1.
    int diff = subimage - current_subimage();
    if (subimage - current_subimage() < 0) {
        // If we are requesting an image earlier than the current one,
        // reset to the first subimage.
        ioseek(0);
        if (!read_header())
            return false;  // read_header always calls error()
        diff = subimage;
    }
    // forward scrolling -- skip subimages until we're at the right place
    while (diff > 0 && m_rla.NextOffset != 0) {
        if (!ioseek(m_rla.NextOffset)) {
            errorfmt("Could not seek to header offset. Corrupted file?");
            return false;
        }
        if (!read_header())
            return false;  // read_header always calls error()
        --diff;
    }
    if (diff > 0 && m_rla.NextOffset == 0) {  // no more subimages to read
        errorfmt("Unknown subimage");
        return false;
    }

    // Now m_rla holds the header of the requested subimage.  Examine it
    // to fill out our ImageSpec.

    if (m_rla.ColorChannelType < 0 || m_rla.ColorChannelType > CT_FLOAT) {
        errorfmt("Illegal color channel type: {}", m_rla.ColorChannelType);
        return false;
    }
    if (m_rla.MatteChannelType < 0 || m_rla.MatteChannelType > CT_FLOAT) {
        errorfmt("Illegal matte channel type: {}", m_rla.MatteChannelType);
        return false;
    }
    if (m_rla.AuxChannelType < 0 || m_rla.AuxChannelType > CT_FLOAT) {
        errorfmt("Illegal auxiliary channel type: {}", m_rla.AuxChannelType);
        return false;
    }

    // pick maximum precision for the time being
    TypeDesc col_type = get_channel_typedesc(m_rla.ColorChannelType,
                                             m_rla.NumOfChannelBits);
    TypeDesc mat_type = m_rla.NumOfMatteChannels
                            ? get_channel_typedesc(m_rla.MatteChannelType,
                                                   m_rla.NumOfMatteBits)
                            : TypeUnknown;
    TypeDesc aux_type = m_rla.NumOfAuxChannels
                            ? get_channel_typedesc(m_rla.AuxChannelType,
                                                   m_rla.NumOfAuxBits)
                            : TypeUnknown;
    TypeDesc maxtype  = TypeDesc::basetype_merge(col_type, mat_type, aux_type);
    if (maxtype == TypeUnknown) {
        errorfmt("Failed channel bytes sanity check");
        return false;  // failed sanity check
    }

    if (m_rla.NumOfColorChannels < 1 || m_rla.NumOfColorChannels > 3
        || m_rla.NumOfMatteChannels < 0 || m_rla.NumOfMatteChannels > 3
        || m_rla.NumOfAuxChannels < 0 || m_rla.NumOfAuxChannels > 256) {
        errorfmt(
            "Invalid number of channels ({} color, {} matte, {} aux), or corrupted header.",
            m_rla.NumOfColorChannels, m_rla.NumOfMatteChannels,
            m_rla.NumOfAuxChannels);
        return false;
    }
    m_spec = ImageSpec(m_rla.ActiveRight - m_rla.ActiveLeft + 1,
                       (m_rla.ActiveTop - m_rla.ActiveBottom + 1)
                           / (m_rla.FieldRendered ? 2 : 1),  // interlaced image?
                       m_rla.NumOfColorChannels + m_rla.NumOfMatteChannels
                           + m_rla.NumOfAuxChannels,
                       maxtype);

    // set window dimensions etc.
    m_spec.x           = m_rla.ActiveLeft;
    m_spec.y           = m_spec.height - 1 - m_rla.ActiveTop;
    m_spec.full_width  = m_rla.WindowRight - m_rla.WindowLeft + 1;
    m_spec.full_height = m_rla.WindowTop - m_rla.WindowBottom + 1;
    m_spec.full_depth  = 1;
    m_spec.full_x      = m_rla.WindowLeft;
    m_spec.full_y      = m_spec.full_height - 1 - m_rla.WindowTop;

    // set channel formats and stride
    int z_channel = -1;
    m_stride      = 0;
    for (int i = 0; i < m_rla.NumOfColorChannels; ++i)
        m_spec.channelformats.push_back(col_type);
    m_stride += m_rla.NumOfColorChannels * col_type.size();
    for (int i = 0; i < m_rla.NumOfMatteChannels; ++i)
        m_spec.channelformats.push_back(mat_type);
    if (m_rla.NumOfMatteChannels >= 1)
        m_spec.alpha_channel = m_rla.NumOfColorChannels;
    else
        m_spec.alpha_channel = -1;
    m_stride += m_rla.NumOfMatteChannels * mat_type.size();
    for (int i = 0; i < m_rla.NumOfAuxChannels; ++i) {
        m_spec.channelformats.push_back(aux_type);
        // assume first float aux or 32 bit int channel is z
        if (z_channel < 0
            && (aux_type == TypeDesc::FLOAT || aux_type == TypeDesc::INT32
                || aux_type == TypeDesc::UINT32)) {
            z_channel = m_rla.NumOfColorChannels + m_rla.NumOfMatteChannels;
            m_spec.z_channel = z_channel;
            if (m_spec.channelnames.size() < size_t(z_channel + 1))
                m_spec.channelnames.resize(z_channel + 1);
            m_spec.channelnames[z_channel] = std::string("Z");
        }
    }
    m_stride += m_rla.NumOfAuxChannels * aux_type.size();

    // But if all channels turned out the same, just use 'format' and don't
    // bother sending back channelformats at all.
    bool allsame = true;
    for (int c = 1; c < m_spec.nchannels; ++c)
        allsame &= (m_spec.channelformats[c] == m_spec.channelformats[0]);
    if (allsame) {
        m_spec.format = m_spec.channelformats[0];
        m_spec.channelformats.clear();
        m_spec.attribute("oiio:BitsPerSample", m_rla.NumOfChannelBits);
        // N.B. don't set bps for mixed formats, it isn't well defined
    }

    // this is always true
    m_spec.attribute("compression", "rle");

    if (m_rla.DateCreated[0]) {
        string_view date(m_rla.DateCreated);
        string_view month = Strutil::parse_word(date);
        int d, h, M, m, y;
        if (month.size() == 3 && Strutil::parse_int(date, d)
            && Strutil::parse_int(date, h) && Strutil::parse_char(date, ':')
            && Strutil::parse_int(date, m) && Strutil::parse_int(date, y)) {
            M = get_month_number(month);
            if (M > 0) {
                // construct a date/time marker in OIIO convention
                m_spec.attribute(
                    "DateTime",
                    Strutil::fmt::format("{:4d}:{:02d}:{:02d} {:02d}:{:02d}:00",
                                         y, M, d, h, m));
            }
        }
    }

    // save some typing by using macros
#define FIELD(x, name) \
    if (m_rla.x > 0)   \
    m_spec.attribute(name, m_rla.x)
#define STRING_FIELD(x, name) \
    if (m_rla.x[0])           \
    m_spec.attribute(name, m_rla.x)

    STRING_FIELD(Description, "ImageDescription");
    FIELD(FrameNumber, "rla:FrameNumber");
    FIELD(Revision, "rla:Revision");
    FIELD(JobNumber, "rla:JobNumber");
    FIELD(FieldRendered, "rla:FieldRendered");
    STRING_FIELD(FileName, "rla:FileName");
    STRING_FIELD(ProgramName, "Software");
    STRING_FIELD(MachineName, "HostComputer");
    STRING_FIELD(UserName, "Artist");
    STRING_FIELD(Aspect, "rla:Aspect");
    STRING_FIELD(ColorChannel, "rla:ColorChannel");
    STRING_FIELD(Time, "rla:Time");
    STRING_FIELD(Filter, "rla:Filter");
    STRING_FIELD(AuxData, "rla:AuxData");
#undef STRING_FIELD
#undef FIELD

    float gamma = Strutil::from_string<float>(m_rla.Gamma);
    if (gamma > 0.f) {
        // Round gamma to the nearest hundredth to prevent stupid
        // precision choices and make it easier for apps to make
        // decisions based on known gamma values. For example, you want
        // 2.2, not 2.19998.
        gamma = roundf(100.0 * gamma) / 100.0f;
        set_colorspace_rec709_gamma(m_spec, gamma);
    }

    float aspect = Strutil::stof(m_rla.AspectRatio);
    if (aspect > 0.f)
        m_spec.attribute("PixelAspectRatio", aspect);

    float f[3];  // variable will be reused for chroma, thus the array
    // read chromaticity points
    if (m_rla.RedChroma[0]) {
        bool three = Strutil::scan_values(m_rla.RedChroma, "",
                                          span<float>(f, 3));
        if (three
            || Strutil::scan_values(m_rla.RedChroma, "", span<float>(f, 2)))
            m_spec.attribute("rla:RedChroma",
                             TypeDesc(TypeDesc::FLOAT,
                                      three ? TypeDesc::VEC3 : TypeDesc::VEC2,
                                      TypeDesc::POINT),
                             f);
    }
    if (m_rla.GreenChroma[0]) {
        bool three = Strutil::scan_values(m_rla.GreenChroma, "",
                                          span<float>(f, 3));
        if (three
            || Strutil::scan_values(m_rla.GreenChroma, "", span<float>(f, 2)))
            m_spec.attribute("rla:GreenChroma",
                             TypeDesc(TypeDesc::FLOAT,
                                      three ? TypeDesc::VEC3 : TypeDesc::VEC2,
                                      TypeDesc::POINT),
                             f);
    }
    if (m_rla.BlueChroma[0]) {
        bool three = Strutil::scan_values(m_rla.BlueChroma, "",
                                          span<float>(f, 3));
        if (three
            || Strutil::scan_values(m_rla.BlueChroma, "", span<float>(f, 2)))
            m_spec.attribute("rla:BlueChroma",
                             TypeDesc(TypeDesc::FLOAT,
                                      three ? TypeDesc::VEC3 : TypeDesc::VEC2,
                                      TypeDesc::POINT),
                             f);
    }
    if (m_rla.WhitePoint[0]) {
        bool three = Strutil::scan_values(m_rla.WhitePoint, "",
                                          span<float>(f, 3));
        if (three
            || Strutil::scan_values(m_rla.WhitePoint, "", span<float>(f, 2)))
            m_spec.attribute("rla:WhitePoint",
                             TypeDesc(TypeDesc::FLOAT,
                                      three ? TypeDesc::VEC3 : TypeDesc::VEC2,
                                      TypeDesc::POINT),
                             f);
    }

    m_subimage = subimage;

    // N.B. the file pointer is now immediately after the scanline
    // offset table for this subimage.
    return true;
}



bool
RLAInput::close()
{
    init();  // Reset to initial state
    return true;
}



size_t
RLAInput::decode_rle_span(unsigned char* buf, int n, int stride,
                          const char* encoded, size_t elen)
{
    size_t e = 0;
    while (n > 0 && e < elen) {
        signed char count = (signed char)encoded[e++];
        if (count >= 0) {
            // run count positive: value repeated count+1 times
            for (int i = 0; i <= count && n && e < elen;
                 ++i, buf += stride, --n)
                *buf = encoded[e];
            ++e;
        } else {
            // run count negative: repeat bytes literally
            count = -count;  // make it positive
            for (; count && n > 0 && e < elen; --count, buf += stride, --n)
                *buf = encoded[e++];
        }
    }
    if (n != 0) {
        errorfmt("Read error: malformed RLE record");
        return 0;
    }
    return e;
}



bool
RLAInput::decode_channel_group(int first_channel, short num_channels,
                               short num_bits, int y)
{
    // Some preliminaries -- figure out various sizes and offsets
    int chsize;         // size of the channels in this group, in bytes
    int offset;         // buffer offset to first channel
    int pixelsize;      // spacing between pixels (in bytes) in the output
    TypeDesc chantype;  // data type for the channel
    if (!m_spec.channelformats.size()) {
        // No per-channel formats, they are all the same, so it's easy
        chantype  = m_spec.format;
        chsize    = chantype.size();
        offset    = first_channel * chsize;
        pixelsize = chsize * m_spec.nchannels;
    } else {
        // Per-channel formats differ, need to sum them up
        chantype  = m_spec.channelformats[first_channel];
        chsize    = chantype.size();
        offset    = 0;
        pixelsize = m_spec.pixel_bytes(true);
        for (int i = 0; i < first_channel; ++i)
            offset += m_spec.channelformats[i].size();
    }

    // Read the big-endian values into the buffer.
    // The channels are simply concatenated together in order.
    // Each channel starts with a length, from which we know how many
    // bytes of encoded RLE data to read.  Then there are RLE
    // spans for each 8-bit slice of the channel.
    std::vector<char> encoded;
    for (int c = 0; c < num_channels; ++c) {
        // Read the length
        uint16_t lenu16;  // number of encoded bytes
        if (!read(&lenu16)) {
            errorfmt("Read error: couldn't read RLE record length");
            return false;
        }
        size_t length = lenu16;
        // Read the encoded RLE record
        encoded.resize(length);
        if (!length || !read(&encoded[0], length)) {
            errorfmt("Read error: couldn't read RLE data span");
            return false;
        }

        if (chantype == TypeDesc::FLOAT) {
            // Special case -- float data is just dumped raw, no RLE
            if (length != size_t(m_spec.width * chsize)) {
                errorfmt(
                    "Read error: not enough data in scanline {}, channel {}", y,
                    c);
                return false;
            }
            for (int x = 0; x < m_spec.width; ++x)
                *((float*)&m_buf[offset + c * chsize + x * pixelsize])
                    = ((float*)&encoded[0])[x];
            continue;
        }

        // Decode RLE -- one pass for each significant byte of the file,
        // which we re-interleave properly by passing the right offsets
        // and strides to decode_rle_span.
        size_t eoffset = 0;
        for (int bytes = 0; bytes < chsize && length > 0; ++bytes) {
            size_t e = decode_rle_span(&m_buf[offset + c * chsize + bytes],
                                       m_spec.width, pixelsize,
                                       &encoded[eoffset], length);
            if (!e)
                return false;
            eoffset += e;
            length -= e;
        }
    }

    // If we're little endian, swap endianness in place for 2- and
    // 4-byte pixel data.
    if (littleendian()) {
        if (chsize == 2) {
            if (num_channels == m_spec.nchannels)
                swap_endian((uint16_t*)&m_buf[0], num_channels * m_spec.width);
            else
                for (int x = 0; x < m_spec.width; ++x)
                    swap_endian((uint16_t*)&m_buf[offset + x * pixelsize],
                                num_channels);
        } else if (chsize == 4 && chantype != TypeDesc::FLOAT) {
            if (num_channels == m_spec.nchannels)
                swap_endian((uint32_t*)&m_buf[0], num_channels * m_spec.width);
            else
                for (int x = 0; x < m_spec.width; ++x)
                    swap_endian((uint32_t*)&m_buf[offset + x * pixelsize],
                                num_channels);
        }
    }

    // If not 8*2^n bits, need to rescale.  For example, if num_bits is
    // 10, the data values run 0-1023, but are stored in uint16.  So we
    // now rescale to the full range of the output buffer range, per
    // OIIO conventions.
    if (num_bits == 8 || num_bits == 16 || num_bits == 32) {
        // ok -- no rescaling needed
    }
    int bytes_per_chan = ceil2(std::max(int(num_bits), 8)) / 8;
    if (size_t(offset + (m_spec.width - 1) * pixelsize
               + num_channels * bytes_per_chan)
        > m_buf.size()) {
        errorfmt("Probably corrupt file (buffer overrun avoided)");
        return false;  // Probably corrupt? Would have overrun
    }
    if (num_bits == 10) {
        // fast, common case -- use templated hard-code
        for (int x = 0; x < m_spec.width; ++x) {
            uint16_t* b = (uint16_t*)(&m_buf[offset + x * pixelsize]);
            for (int c = 0; c < num_channels; ++c)
                b[c] = bit_range_convert<10, 16>(b[c]);
        }
    } else if (num_bits < 8) {
        // rare case, use slow code to make this clause short and simple
        for (int x = 0; x < m_spec.width; ++x) {
            uint8_t* b = (uint8_t*)&m_buf[offset + x * pixelsize];
            for (int c = 0; c < num_channels; ++c)
                b[c] = bit_range_convert(b[c], num_bits, 8);
        }
    } else if (num_bits > 8 && num_bits < 16) {
        // rare case, use slow code to make this clause short and simple
        for (int x = 0; x < m_spec.width; ++x) {
            uint16_t* b = (uint16_t*)&m_buf[offset + x * pixelsize];
            for (int c = 0; c < num_channels; ++c)
                b[c] = bit_range_convert(b[c], num_bits, 16);
        }
    } else if (num_bits > 16 && num_bits < 32) {
        // rare case, use slow code to make this clause short and simple
        for (int x = 0; x < m_spec.width; ++x) {
            uint32_t* b = (uint32_t*)&m_buf[offset + x * pixelsize];
            for (int c = 0; c < num_channels; ++c)
                b[c] = bit_range_convert(b[c], num_bits, 32);
        }
    }
    return true;
}



bool
RLAInput::read_native_scanline(int subimage, int miplevel, int y, int /*z*/,
                               void* data)
{
    lock_guard lock(*this);
    if (!seek_subimage(subimage, miplevel))
        return false;

    // By convention, RLA images store their images bottom-to-top.
    y = m_spec.height - (y - m_spec.y) - 1;

    // Seek to scanline start, based on the scanline offset table
    ioseek(m_sot[y]);

    // Now decode and interleave the channels.
    // The channels are non-interleaved (i.e. rrrrrgggggbbbbb...).
    // Color first, then matte, then auxiliary channels.  We can't
    // decode all in one shot, though, because the data type and number
    // of significant bits may be may be different for each class of
    // channels, so we deal with them separately and interleave into
    // our buffer as we go.
    size_t size = m_spec.scanline_bytes(true);
    m_buf.resize(size);
    if (m_rla.NumOfColorChannels > 0)
        if (!decode_channel_group(0, m_rla.NumOfColorChannels,
                                  m_rla.NumOfChannelBits, y))
            return false;
    if (m_rla.NumOfMatteChannels > 0)
        if (!decode_channel_group(m_rla.NumOfColorChannels,
                                  m_rla.NumOfMatteChannels,
                                  m_rla.NumOfMatteBits, y))
            return false;
    if (m_rla.NumOfAuxChannels > 0)
        if (!decode_channel_group(m_rla.NumOfColorChannels
                                      + m_rla.NumOfMatteChannels,
                                  m_rla.NumOfAuxChannels, m_rla.NumOfAuxBits,
                                  y))
            return false;

    memcpy(data, &m_buf[0], size);
    return true;
}



inline int
RLAInput::get_month_number(string_view s)
{
    static const char* months[] = { "",    "jan", "feb", "mar", "apr",
                                    "may", "jun", "jul", "aug", "sep",
                                    "oct", "nov", "dec" };
    for (int i = 1; i <= 12; ++i)
        if (Strutil::iequals(s, months[i]))
            return i;
    return -1;
}



inline TypeDesc
RLAInput::get_channel_typedesc(short chan_type, short chan_bits)
{
    switch (chan_type) {
    case CT_BYTE:
        // some non-spec-compliant images > 8bpc will have it set to
        // byte anyway, so try guessing by bit depth instead
        if (chan_bits > 8) {
            switch ((chan_bits + 7) / 8) {
            case 2: return TypeDesc::UINT16;
            case 3:
            case 4: return TypeDesc::UINT32;
            default: OIIO_ASSERT(!"Invalid colour channel type");
            }
        } else
            return TypeDesc::UINT8;
    case CT_WORD: return TypeDesc::UINT16;
    case CT_DWORD: return TypeDesc::UINT32;
    case CT_FLOAT: return TypeDesc::FLOAT;
    default: OIIO_ASSERT(!"Invalid colour channel type");
    }
    // shut up compiler
    return TypeDesc::UINT8;
}

OIIO_PLUGIN_NAMESPACE_END
