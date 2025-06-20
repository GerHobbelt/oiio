// Copyright Contributors to the OpenImageIO project.
// SPDX-License-Identifier: Apache-2.0
// https://github.com/AcademySoftwareFoundation/OpenImageIO


#include <cstring>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <OpenImageIO/Imath.h>

#include <OpenImageIO/color.h>
#include <OpenImageIO/dassert.h>
#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/fmath.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/imagecache.h>
#include <OpenImageIO/imageio.h>
#include <OpenImageIO/optparser.h>
#include <OpenImageIO/simd.h>
#include <OpenImageIO/strutil.h>
#include <OpenImageIO/sysutil.h>
#include <OpenImageIO/texture.h>
#include <OpenImageIO/thread.h>
#include <OpenImageIO/timer.h>
#include <OpenImageIO/typedesc.h>
#include <OpenImageIO/ustring.h>

#include "imagecache_memory_print.h"
#include "imagecache_memory_pvt.h"
#include "imagecache_pvt.h"
#include "imageio_pvt.h"


OIIO_NAMESPACE_BEGIN
using namespace pvt;
using ImageDims    = ImageCacheFile::ImageDims;
using LevelInfo    = ImageCacheFile::LevelInfo;
using SubimageInfo = ImageCacheFile::SubimageInfo;

// The static perthread mutex needs to outlive the shared_image_cache
// instance, so must be declared first in this file to avoid static
// initialization order problems.
spin_mutex ImageCacheImpl::m_perthread_info_mutex;


namespace {  // anonymous

static thread_local tsl::robin_map<uint64_t, std::string> imcache_error_messages;
static std::shared_ptr<ImageCache> shared_image_cache;
static spin_mutex shared_image_cache_mutex;

// Make some static ustring constants to avoid strcmp's
static ustring s_resolution("resolution"), s_texturetype("texturetype");
static ustring s_textureformat("textureformat"), s_fileformat("fileformat");
static ustring s_format("format"), s_cachedformat("cachedformat");
static ustring s_channels("channels"), s_cachedpixeltype("cachedpixeltype");
static ustring s_exists("exists"), s_broken("broken");
static ustring s_UDIM("UDIM");
static ustring s_subimages("subimages"), s_miplevels("miplevels");
static ustring s_datawindow("datawindow"), s_displaywindow("displaywindow");
static ustring s_averagecolor("averagecolor"), s_averagealpha("averagealpha");
static ustring s_constantcolor("constantcolor");
static ustring s_constantalpha("constantalpha");

// constantly increasing, so we can avoid issues if an ImageCache pointer is re-used after being freed
static std::atomic_int64_t imagecache_next_id = 0;
static thread_local tsl::robin_map<uint64_t, ImageCachePerThreadInfo*>
    imagecache_per_thread_infos;


// Functor to compare filenames
static bool
filename_compare(const ImageCacheFileRef& a, const ImageCacheFileRef& b)
{
    return a->filename() < b->filename();
}


// Functor to compare read bytes, sort in descending order
static bool
bytesread_compare(const ImageCacheFileRef& a, const ImageCacheFileRef& b)
{
    return a->bytesread() > b->bytesread();
}


// Functor to compare read times, sort in descending order
static bool
iotime_compare(const ImageCacheFileRef& a, const ImageCacheFileRef& b)
{
    return a->iotime() > b->iotime();
}


// Functor to compare read rate (MB/s), sort in ascending order
static bool
iorate_compare(const ImageCacheFileRef& a, const ImageCacheFileRef& b)
{
    double arate = (a->iotime() == 0)
                       ? 0
                       : a->bytesread() / (1024.0 * 1024.0) / a->iotime();
    double brate = (b->iotime() == 0)
                       ? 0
                       : b->bytesread() / (1024.0 * 1024.0) / b->iotime();
    return arate < brate;
}


// Functor to compare amount of redundant reading, sort in descending order
static bool
redundantbytes_compare(const ImageCacheFileRef& a, const ImageCacheFileRef& b)
{
    return a->redundant_bytesread() > b->redundant_bytesread();
}


};  // end anonymous namespace



void
ImageCacheStatistics::init()
{
    // ImageCache stats:
    find_tile_calls             = 0;
    find_tile_microcache_misses = 0;
    find_tile_cache_misses      = 0;
    //    tiles_created = 0;
    //    tiles_current = 0;
    //    tiles_peak = 0;
    files_totalsize        = 0;
    files_totalsize_ondisk = 0;
    bytes_read             = 0;
    //    open_files_created = 0;
    //    open_files_current = 0;
    //    open_files_peak = 0;
    unique_files      = 0;
    fileio_time       = 0;
    fileopen_time     = 0;
    file_locking_time = 0;
    tile_locking_time = 0;
    find_file_time    = 0;
    find_tile_time    = 0;

    // TextureSystem stats:
    texture_queries     = 0;
    texture_batches     = 0;
    texture3d_queries   = 0;
    texture3d_batches   = 0;
    shadow_queries      = 0;
    shadow_batches      = 0;
    environment_queries = 0;
    environment_batches = 0;
    imageinfo_queries   = 0;
    aniso_queries       = 0;
    aniso_probes        = 0;
    max_aniso           = 1;
    closest_interps     = 0;
    bilinear_interps    = 0;
    cubic_interps       = 0;
    file_retry_success  = 0;
    tile_retry_success  = 0;
}



void
ImageCacheStatistics::merge(const ImageCacheStatistics& s)
{
    // ImageCache stats:
    find_tile_calls += s.find_tile_calls;
    find_tile_microcache_misses += s.find_tile_microcache_misses;
    find_tile_cache_misses += s.find_tile_cache_misses;
    //    tiles_created += s.tiles_created;
    //    tiles_current += s.tiles_current;
    //    tiles_peak += s.tiles_peak;
    files_totalsize += s.files_totalsize;
    files_totalsize_ondisk += s.files_totalsize_ondisk;
    bytes_read += s.bytes_read;
    //    open_files_created += s.open_files_created;
    //    open_files_current += s.open_files_current;
    //    open_files_peak += s.open_files_peak;
    unique_files += s.unique_files;
    fileio_time += s.fileio_time;
    fileopen_time += s.fileopen_time;
    file_locking_time += s.file_locking_time;
    tile_locking_time += s.tile_locking_time;
    find_file_time += s.find_file_time;
    find_tile_time += s.find_tile_time;

    // TextureSystem stats:
    texture_queries += s.texture_queries;
    texture_batches += s.texture_batches;
    texture3d_queries += s.texture3d_queries;
    texture3d_batches += s.texture3d_batches;
    shadow_queries += s.shadow_queries;
    shadow_batches += s.shadow_batches;
    environment_queries += s.environment_queries;
    environment_batches += s.environment_batches;
    imageinfo_queries += s.imageinfo_queries;
    aniso_queries += s.aniso_queries;
    aniso_probes += s.aniso_probes;
    max_aniso = std::max(max_aniso, s.max_aniso);
    closest_interps += s.closest_interps;
    bilinear_interps += s.bilinear_interps;
    cubic_interps += s.cubic_interps;
    file_retry_success += s.file_retry_success;
    tile_retry_success += s.tile_retry_success;
}



template<typename T>
inline bool
has_same_dimensions(const T& a, const T& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z && a.width == b.width
           && a.height == b.height && a.depth == b.depth && a.full_x == b.full_x
           && a.full_y == b.full_y && a.full_z == b.full_z
           && a.full_width == b.full_width && a.full_height == b.full_height
           && a.full_depth == b.full_depth && a.tile_width == b.tile_width
           && a.tile_height == b.tile_height && a.tile_depth == b.tile_depth
           && a.nchannels == b.nchannels;
}



inline bool
is_same(const ImageSpec& a, const ImageSpec& b)
{
    if (!has_same_dimensions(a, b))
        return false;
    if (a.alpha_channel != b.alpha_channel)
        return false;
    if (a.z_channel != b.z_channel)
        return false;
    if (a.deep != b.deep)
        return false;
    if (a.format != b.format)
        return false;
    if (a.channelformats.size() != b.channelformats.size())
        return false;
    if (a.channelnames.size() != b.channelnames.size())
        return false;
    if (a.extra_attribs.size() != b.extra_attribs.size())
        return false;

    for (size_t i = 0; i < a.channelformats.size(); ++i)
        if (a.channelformats.at(i) != b.channelformats.at(i))
            return false;

    for (size_t i = 0; i < a.channelnames.size(); ++i)
        if (a.channelnames.at(i) != b.channelnames.at(i))
            return false;

    for (size_t i = 0; i < a.extra_attribs.size(); ++i) {
        const ParamValue& pa = a.extra_attribs.at(i);
        const ParamValue& pb = b.extra_attribs.at(i);
        //! compare attribute infos
        if (pa.name() != pb.name())
            return false;
        if (pa.type() != pb.type())
            return false;
        if (pa.nvalues() != pb.nvalues())
            return false;
        if (pa.datasize() != pb.datasize())
            return false;
        //! compare attribute memory
        if (std::memcmp(pa.data(), pb.data(), pa.datasize()) != 0)
            return false;
    }
    return true;
}



template<typename T>
inline bool
has_full_pixel_range(const T& s)
{
    return s.x == s.full_x && s.y == s.full_y && s.z == s.full_z
           && s.width == s.full_width && s.height == s.full_height
           && s.depth == s.full_depth;
}



template<typename T>
inline bool
has_one_tile(const T& s)
{
    return s.height <= s.tile_height && s.width <= s.tile_width
           && s.depth <= s.tile_depth;
}



LevelInfo::LevelInfo(ImageSpec* spec_, ImageDims* dims_)
    : m_dims(dims_)
    , nxtiles(1)
    , nytiles(1)
    , nztiles(1)
{
    OIIO_DASSERT(spec_);
    const ImageDims& dims = m_dims ? *m_dims : ImageDims::convert(*spec_);
    nchannels             = dims.nchannels;
    full_pixel_range      = has_full_pixel_range(dims);
    onetile               = has_one_tile(dims);
    polecolorcomputed     = false;
    // Allocate bit field for which tiles have been read at least once.
    if (!onetile) {
        nxtiles = (dims.width + dims.tile_width - 1) / dims.tile_width;
        nytiles = (dims.height + dims.tile_height - 1) / dims.tile_height;
        nztiles = (dims.depth + dims.tile_depth - 1) / dims.tile_depth;
    }
    int total_tiles = nxtiles * nytiles * nztiles;
    OIIO_DASSERT(total_tiles >= 1);
    const int sz = round_to_multiple(total_tiles, 64) / 64;
    tiles_read   = new atomic_ll[sz];
    for (int i = 0; i < sz; i++)
        tiles_read[i] = 0;
}



LevelInfo::LevelInfo(const LevelInfo& src)
    : m_dims(src.m_dims)
    , nxtiles(src.nxtiles)
    , nytiles(src.nytiles)
    , nztiles(src.nztiles)
    , nchannels(src.nchannels)
    , full_pixel_range(src.full_pixel_range)
    , onetile(src.onetile)
    , polecolorcomputed(src.polecolorcomputed)
{
    if (src.polecolor) {
        polecolor.reset(new float[2 * nchannels]);
        std::copy_n(src.polecolor.get(), 2 * nchannels, polecolor.get());
    }
    int nwords = round_to_multiple(nxtiles * nytiles * nztiles, 64) / 64;
    tiles_read = new atomic_ll[nwords];
    for (int i = 0; i < nwords; ++i)
        tiles_read[i] = src.tiles_read[i].load();
}



ImageCacheFile::ImageCacheFile(ImageCacheImpl& imagecache,
                               ImageCachePerThreadInfo* /*thread_info*/,
                               ustring filename, ImageInput::Creator creator,
                               const ImageSpec* config)
    : m_filename(filename)
    , m_used(true)
    , m_broken(false)
    , m_texformat(TexFormatTexture)
    , m_swrap(TextureOpt::WrapBlack)
    , m_twrap(TextureOpt::WrapBlack)
    , m_rwrap(TextureOpt::WrapBlack)
    , m_envlayout(LayoutTexture)
    , m_y_up(false)
    , m_sample_border(false)
    , m_udim_nutiles(0)
    , m_udim_nvtiles(0)
    , m_tilesread(0)
    , m_bytesread(0)
    , m_redundant_tiles(0)
    , m_redundant_bytesread(0)
    , m_timesopened(0)
    , m_iotime(0)
    , m_mutex_wait_time(0)
    , m_mipused(false)
    , m_validspec(false)
    , m_errors_issued(0)
    , m_imagecache(imagecache)
    , m_duplicate(NULL)
    , m_total_imagesize(0)
    , m_total_imagesize_ondisk(0)
    , m_inputcreator(creator)
    , m_configspec(config ? new ImageSpec(*config) : NULL)
{
    m_filename_original = m_filename;
    m_filename = imagecache.resolve_filename(m_filename_original.string());
    // N.B. the file is not opened, the ImageInput is NULL.  This is
    // reflected by the fact that m_validspec is false.

    // Figure out if it's a UDIM-like virtual texture
    udim_setup();

    // If the config has an IOProxy, remember that we should never actually
    // release() it, because the proxy can't be reopened.
    if (config && config->find_attribute("oiio:ioproxy", TypeDesc::PTR))
        m_allow_release = false;
}



ImageCacheFile::~ImageCacheFile() { close(); }



void
ImageCacheFile::reset(ImageInput::Creator creator, const ImageSpec* config)
{
    m_inputcreator = creator;
    m_configspec.reset(config ? new ImageSpec(*config) : nullptr);
}



std::shared_ptr<ImageInput>
ImageCacheFile::get_imageinput(ImageCachePerThreadInfo* /*thread_info*/)
{
#if defined(__GLIBCXX__) && __GLIBCXX__ < 20160822
    // Older gcc libstdc++ does not properly support std::atomic
    // operations on std::shared_ptr, despite it being a C++11
    // feature. No choice but to lock.
    recursive_timed_lock_guard guard(m_input_mutex);
    return m_input;
#else
    // True C++11: can atomically load a shared_ptr safely.
    return std::atomic_load(&m_input);
#endif
}



void
ImageCacheFile::set_imageinput(std::shared_ptr<ImageInput> newval)
{
    if (newval)
        imagecache().incr_open_files();
#if defined(__GLIBCXX__) && __GLIBCXX__ < 20160822
    // Older gcc libstdc++ does not properly support std::atomic
    // operations on std::shared_ptr, despite it being a C++11
    // feature. No choice but to lock.
    std::shared_ptr<ImageInput> oldval;
    {
        recursive_timed_lock_guard guard(m_input_mutex);
        oldval  = m_input;
        m_input = newval;
    }
#else
    // True C++11: can atomically exchange a shared_ptr safely.
    auto oldval = std::atomic_exchange(&m_input, newval);
#endif
    if (oldval)
        imagecache().decr_open_files();
}



void
ImageCacheFile::SubimageInfo::init(ImageCacheFile& icfile, ImageSpec* spec_,
                                   bool forcefloat)
{
    m_spec = spec_;
    OIIO_DASSERT(m_spec);
    const ImageSpec& spec = *m_spec;
    volume                = (spec.depth > 1 || spec.full_depth > 1);
    full_pixel_range      = has_full_pixel_range(spec);
    if (!full_pixel_range) {
        sscale  = float(spec.full_width) / spec.width;
        soffset = float(spec.full_x - spec.x) / spec.width;
        tscale  = float(spec.full_height) / spec.height;
        toffset = float(spec.full_y - spec.y) / spec.height;
    } else {
        sscale = tscale = 1.0f;
        soffset = toffset = 0.0f;
    }
    subimagename = ustring(spec.get_string_attribute("oiio:subimagename"));
    datatype     = TypeDesc::FLOAT;
    if (!forcefloat) {
        // If we aren't forcing everything to be float internally, then
        // there are a few other types we allow.
        if (spec.format == TypeDesc::UINT8 || spec.format == TypeDesc::UINT16
            || spec.format == TypeDesc::HALF
            /* future expansion:  || spec.format == AnotherFormat ... */)
            datatype = spec.format;
    }
    channelsize = datatype.size();
    pixelsize   = channelsize * spec.nchannels;

    // See if there's a constant color tag
    string_view software = spec.get_string_attribute("Software");
    bool from_maketx     = Strutil::istarts_with(software, "OpenImageIO")
                       || Strutil::istarts_with(software, "maketx");

    string_view constant_color = spec.get_string_attribute(
        "oiio:ConstantColor");
    if (from_maketx && constant_color.size()) {
        while (constant_color.size()) {
            float val;
            if (!Strutil::parse_float(constant_color, val))
                break;
            average_color.push_back(val);
            if (!Strutil::parse_char(constant_color, ','))
                break;
        }
        if (average_color.size() == size_t(spec.nchannels)) {
            is_constant_image = true;
            has_average_color = true;
        }
    }

    // See if there's an average color tag
    string_view avgcolor = spec.get_string_attribute("oiio:AverageColor");
    if (from_maketx && avgcolor.size()) {
        while (avgcolor.size()) {
            float val;
            if (!Strutil::parse_float(avgcolor, val))
                break;
            average_color.push_back(val);
            if (!Strutil::parse_char(avgcolor, ','))
                break;
        }
        if (average_color.size() == size_t(spec.nchannels))
            has_average_color = true;
    }

    const ParamValue* p = spec.find_attribute("worldtolocal", TypeMatrix);
    if (p) {
        Imath::M44f c2w;
        icfile.m_imagecache.get_commontoworld(c2w);
        const Imath::M44f* m = (const Imath::M44f*)p->data();
        Mlocal.reset(new Imath::M44f(c2w * (*m)));
    }

    // Try to preallocate if the number of miplevels is known beforehand
    int nmiplevels = spec.get_int_attribute("oiio:miplevels", 0);
    if (nmiplevels > 0)
        levels.reserve(nmiplevels);
}



imagesize_t
ImageCacheFile::SubimageInfo::get_tile_pixels(int m) const
{
    const ImageDims& ls = leveldims(m);
    if (ls.tile_width <= 0 || ls.tile_height <= 0 || ls.tile_depth <= 0)
        return 0;
    imagesize_t r = clamped_mult64((imagesize_t)ls.tile_width,
                                   (imagesize_t)ls.tile_height);
    if (ls.tile_depth > 1)
        r = clamped_mult64(r, (imagesize_t)ls.tile_depth);
    return r;
}



size_t
ImageCacheFile::SubimageInfo::get_pixel_bytes() const
{
    const ImageSpec& s = spec();
    //! TODO: maybe this shouldn't ever happen in the image cache and we
    // could get rid of the condition
    if (s.nchannels <= 0)
        return 0;
    return clamped_mult32((size_t)s.nchannels, s.format.size());
}



imagesize_t
ImageCacheFile::SubimageInfo::get_tile_bytes(int m) const
{
    return clamped_mult64(get_tile_pixels(m), (imagesize_t)get_pixel_bytes());
}



imagesize_t
ImageCacheFile::SubimageInfo::get_scanline_bytes(int m) const
{
    const ImageDims& ls = leveldims(m);
    if (ls.width < 0)
        return 0;
    return clamped_mult64((imagesize_t)ls.width,
                          (imagesize_t)get_pixel_bytes());
}



imagesize_t
ImageCacheFile::SubimageInfo::get_image_pixels(int m) const
{
    const ImageDims& ls = leveldims(m);
    if (ls.width < 0 || ls.height < 0 || ls.depth < 0)
        return 0;
    imagesize_t r = clamped_mult64((imagesize_t)ls.width,
                                   (imagesize_t)ls.height);
    if (ls.depth > 1)
        r = clamped_mult64(r, (imagesize_t)ls.depth);
    return r;
}



imagesize_t
ImageCacheFile::SubimageInfo::get_image_bytes(int m) const
{
    return clamped_mult64(get_image_pixels(m), (imagesize_t)get_pixel_bytes());
}



ImageSpec*
ImageCacheFile::find_spec(int subimage, const ImageSpec& spec)
{
    constexpr int N = 10;
    int p           = 1;
    while (p <= N && subimage - p >= 0) {
        const int subid = subimage - p;
        const SubimageInfo& si(subimageinfo(subid));
        if (si.m_spec && is_same(*si.m_spec, spec))
            return si.m_spec;
        p++;
    }
    return nullptr;
}



ImageSpec*
ImageCacheFile::find_or_create_spec(int subimage, const ImageSpec& spec)
{
    // next we try to deduplicate ImageSpec across subimages
    ImageSpec* tmp = nullptr;
    if (enable_specs_reuse && subimage > 0)
        tmp = find_spec(subimage, spec);
    // if we cannot reuse a previously allocated ImageSpec, just create one
    if (!tmp) {
        m_pool_specs.emplace_back(std::make_unique<ImageSpec>(spec));
        tmp = m_pool_specs.back().get();
    }
    return tmp;
}



ImageDims*
ImageCacheFile::find_dims(int subimage, int miplevel, const ImageDims& dims)
{
    constexpr int N = 10;
    int p           = 1;
    while (p <= N && subimage - p >= 0) {
        const int subid = subimage - p;
        p++;
        if (miplevel < subimageinfo(subid).miplevels()) {
            const LevelInfo& lvl(levelinfo(subid, miplevel));
            if (lvl.m_dims && has_same_dimensions(*lvl.m_dims, dims))
                return lvl.m_dims;
        }
    }
    return nullptr;
}



ImageDims*
ImageCacheFile::find_or_create_dims(int subimage, int miplevel,
                                    const ImageSpec& spec)
{
    // next we try to deduplicate ImageDims across subimages
    ImageDims* tmp = nullptr;
    if (enable_dims_reuse && subimage > 0)
        tmp = find_dims(subimage, miplevel, ImageDims::convert(spec));
    // if we cannot reuse a previously allocated ImageDims, just create one
    if (!tmp) {
        m_pool_dims.emplace_back(
            std::make_unique<ImageDims>(ImageDims::convert(spec)));
        tmp = m_pool_dims.back().get();
    }
    return tmp;
}



bool
ImageCacheFile::init_texture_format(const ImageSpec& spec)
{
    bool clamp_texture_dims = false;
    if (spec.depth <= 1 && spec.full_depth <= 1)
        m_texformat = TexFormatTexture;
    else
        m_texformat = TexFormatTexture3d;
    const ParamValue* p;
    if ((p = spec.find_attribute("textureformat", TypeDesc::STRING))) {
        const char* textureformat = *(const char**)p->data();
        for (int i = 0; i < TexFormatLast; ++i)
            if (Strutil::iequals(textureformat,
                                 texture_format_name((TexFormat)i))) {
                m_texformat = (TexFormat)i;
                break;
            }
        clamp_texture_dims = (m_texformat == TexFormatTexture);
    }
    return clamp_texture_dims;
}



inline void
clamp_dimensions(ImageSpec& spec)
{
    if (spec.full_width > spec.width)
        spec.full_width = spec.width;
    if (spec.full_height > spec.height)
        spec.full_height = spec.height;
    if (spec.full_depth > spec.depth)
        spec.full_depth = spec.depth;
}



std::shared_ptr<ImageInput>
ImageCacheFile::open(ImageCachePerThreadInfo* thread_info)
{
    // Simple case -- no lock needed: atomically retrieve a shared pointer
    // to the ImageInput. If it exists, return it. Unless the file is
    // marked as broken, in which case return an empty pointer.
    std::shared_ptr<ImageInput> inp = get_imageinput(thread_info);
    if (m_broken)
        return {};
    if (inp)
        return inp;

    // The file wasn't already opened and in a good state.

    // Enforce limits on maximum number of open files.
    imagecache().check_max_files(thread_info);

    // Now we do a lock and open it for real, to keep anybody else from
    // going through the whole opening process simultaneously.
    Timer input_mutex_timer;
    recursive_timed_lock_guard guard(m_input_mutex);
    m_mutex_wait_time += input_mutex_timer();

    // JUST IN CASE somebody else opened the file we want, between when we
    // checked and when we acquired the lock, check again.
    //
    // FIXME: do we care? Is it better to block, or is it better to
    // redundantly open the file and one will get quickly closed and
    // discarded?
    inp = get_imageinput(thread_info);
    if (m_broken)
        return {};
    if (inp)
        return inp;

    ImageSpec configspec;
    if (m_configspec)
        configspec = *m_configspec;
    if (imagecache().unassociatedalpha())
        configspec.attribute("oiio:UnassociatedAlpha", 1);

    if (m_inputcreator)
        inp.reset(m_inputcreator());
    else {
        // If we are trusting extensions and this isn't a special "REST-ful"
        // name construction, just open with the extension in order to skip
        // an unnecessary file open.
        std::string fmt;
        if (m_imagecache.trust_file_extensions()
            && m_filename.find('?') != m_filename.npos)
            fmt = OIIO::Filesystem::extension(fmt, false);
        else
            fmt = m_filename.string();
        inp = ImageInput::create(fmt, false, &configspec, nullptr,
                                 m_imagecache.plugin_searchpath());
    }

    //! helper: use `return invalid_file("error message")` whenever an error occurs
    auto invalid_file = [&](string_view error) -> std::shared_ptr<ImageInput> {
        inp.reset();
        mark_broken(error);  // Note: this already calls invalidate_spec()
        return {};
    };

    if (!inp)
        return invalid_file(OIIO::geterror());

    ImageSpec nativespec, tempspec;
    mark_not_broken();
    bool ok = true;
    for (int tries = 0; tries <= imagecache().failure_retries(); ++tries) {
        ok = inp->open(m_filename.c_str(), nativespec, configspec);
        if (ok) {
            tempspec = nativespec;
            if (tries)  // succeeded, but only after a failure!
                ++thread_info->m_stats.file_retry_success;
            (void)inp->geterror();  // Eat the errors
            break;
        }
        if (tries < imagecache().failure_retries()) {
            // We failed, but will wait a bit and try again.
            Sysutil::usleep(1000 * 100);  // 100 ms
        }
    }
    if (!ok)
        return invalid_file(inp->geterror());

    m_fileformat = ustring(inp->format_name());
    ++m_timesopened;
    use();

    // If we are simply re-opening a closed file, and the spec is still
    // valid, we're done, no need to reread the subimage and mip headers.
    if (validspec()) {
        set_imageinput(inp);
        return inp;
    }

    // From here on, we know that we've opened this file for the very
    // first time.  So read all the subimages, fill out all the fields
    // of the ImageCacheFile.
    m_subimages.clear();
    // Try to preallocate if the number of subimages is known beforehand
    int nsubimages = nativespec.get_int_attribute("oiio:subimages", 0);
    if (nsubimages > 0)
        m_subimages.reserve(nsubimages);
    // reset subimage counter to zero
    nsubimages = 0;

    // Since each subimage can potentially have its own mipmap levels,
    // keep track of the highest level discovered
    imagesize_t old_total_imagesize        = m_total_imagesize;
    imagesize_t old_total_imagesize_ondisk = m_total_imagesize_ondisk;
    m_total_imagesize                      = 0;
    do {
        m_subimages.resize(nsubimages + 1);
        SubimageInfo& si(subimageinfo(nsubimages));
        int max_mip_res         = imagecache().max_mip_res();
        int nmip                = 0;
        ImageSpec* sispec       = nullptr;
        bool clamp_texture_dims = false;
        do {
            nativespec = inp->spec(nsubimages, nmip);
            if (nativespec.nchannels > std::numeric_limits<uint16_t>::max())
                return invalid_file(
                    "Images with more than 65535 channels are not supported.");
            tempspec = nativespec;
            if (nmip == 0) {
                sispec = find_or_create_spec(nsubimages, tempspec);
                OIIO_DASSERT(sispec);
                // Things to do on MIP level 0, i.e. once per subimage
                si.init(*this, sispec, imagecache().forcefloat());
                // Update file texture format at the first subimage
                // and clamp texture dimensions if required
                if (nsubimages == 0) {
                    clamp_texture_dims = init_texture_format(*sispec);
                    if (clamp_texture_dims)
                        clamp_dimensions(*sispec);
                }
            }
            if (tempspec.tile_width == 0 || tempspec.tile_height == 0) {
                si.untiled   = true;
                int autotile = imagecache().autotile();
                // Special consideration: for TIFF files, there is extra
                // efficiency from making our fake tiles correspond to the
                // strips. (N.B.: for non-TIFF files, this attribute will
                // be absent and thus default to 0.)
                int rps = tempspec["tiff:RowsPerStrip"].get<int>();
                if (rps > 1)
                    autotile = round_to_multiple(std::max(autotile, 64), rps);
                si.autotiled = (autotile != 0);
                if (autotile) {
                    // Automatically make it appear as if it's tiled
                    if (imagecache().autoscanline() || rps > 1) {
                        tempspec.tile_width = tempspec.width;
                    } else {
                        tempspec.tile_width = std::min(tempspec.width,
                                                       autotile);
                    }
                    tempspec.tile_height = std::min(tempspec.height, autotile);
                    tempspec.tile_depth  = std::min(std::max(tempspec.depth, 1),
                                                    autotile);
                } else {
                    // Don't auto-tile -- which really means, make it look like
                    // a single tile that's as big as the whole image.
                    tempspec.tile_width  = tempspec.width;
                    tempspec.tile_height = tempspec.height;
                    tempspec.tile_depth  = tempspec.depth;
                }
            }
            // If a request was made for a maximum MIP resolution to use for
            // texture lookups and this level is too big, bump up this
            // subimage's "min MIP level".
            if (tempspec.width > max_mip_res || tempspec.height > max_mip_res
                || tempspec.depth > max_mip_res)
                si.min_mip_level = nmip + 1;
            //            thread_info->m_stats.files_totalsize += tempspec.image_bytes();
            m_total_imagesize += tempspec.image_bytes();
            // All MIP levels need the same number of channels
            if (nmip > 0 && tempspec.nchannels != sispec->nchannels)
                // No idea what to do with a subimage that doesn't have the
                // same number of channels as the others, so just skip it.
                return invalid_file(
                    "Subimages don't all have the same number of channels");

            // ImageCache can't store differing formats per channel
            tempspec.channelformats.clear();
            // Clamp mip level texture dimensions if required
            if (clamp_texture_dims)
                clamp_dimensions(tempspec);
            // Each mip level stores only the fields that differ from the native spec
            OIIO_DASSERT(sispec);
            ImageDims* dims = nullptr;
            if (!has_same_dimensions(*sispec, tempspec)) {
                dims = find_or_create_dims(nsubimages, nmip, tempspec);
                OIIO_DASSERT(dims);
            }
            si.levels.emplace_back(LevelInfo(sispec, dims));
            ++nmip;
        } while (inp->seek_subimage(nsubimages, nmip));

        // Special work for non-MIPmapped images -- but only if "automip"
        // is on, it's a non-mipmapped image, and it doesn't have a
        // "textureformat" attribute (because that would indicate somebody
        // constructed it as texture and specifically wants it un-mipmapped).
        // But not volume textures -- don't auto MIP them for now.
        if (nmip == 1 && !si.volume
            && (tempspec.width > 1 || tempspec.height > 1 || tempspec.depth > 1))
            si.unmipped = true;
        if (si.unmipped && imagecache().automip()
            && !tempspec.find_attribute("textureformat", TypeString)) {
            int w = tempspec.full_width;
            int h = tempspec.full_height;
            int d = tempspec.full_depth;
            while (w > 1 || h > 1 || d > 1) {
                w             = std::max(1, w / 2);
                h             = std::max(1, h / 2);
                d             = std::max(1, d / 2);
                ImageSpec s   = tempspec;
                s.width       = w;
                s.height      = h;
                s.depth       = d;
                s.full_width  = w;
                s.full_height = h;
                s.full_depth  = d;
                if (imagecache().autotile()) {
                    if (imagecache().autoscanline()) {
                        s.tile_width = w;
                    } else {
                        s.tile_width = std::min(imagecache().autotile(), w);
                    }
                    s.tile_height = std::min(imagecache().autotile(), h);
                    s.tile_depth  = std::min(imagecache().autotile(), d);
                } else {
                    s.tile_width  = w;
                    s.tile_height = h;
                    s.tile_depth  = d;
                }
                ++nmip;
                OIIO_DASSERT(sispec);
                ImageDims* dims = nullptr;
                if (!has_same_dimensions(*sispec, s)) {
                    dims = find_or_create_dims(nsubimages, nmip, s);
                    OIIO_DASSERT(dims);
                }
                si.levels.emplace_back(LevelInfo(sispec, dims));
            }
        }
        if (si.untiled && !imagecache().accept_untiled())
            return invalid_file("image was untiled");
        if (si.unmipped && !imagecache().accept_unmipped() &&
            // Allow unmip-mapped for null inputs (user buffers)
            strcmp(inp->format_name(), "null"))
            return invalid_file("image was not MIP-mapped");
        si.n_mip_levels = nmip;
        // Make sure we didn't set the subimage's mip_mip_level past the end
        si.min_mip_level = std::min(si.min_mip_level, nmip - 1);
        // Precompute the min(width,height) for each mip levels without
        // needing to rifle through the imagespecs each time.
        si.minwh.reset(new int[nmip + 1]);
        for (int m = 0; m < nmip; ++m)
            si.minwh[m] = std::min(si.leveldims(m).width,
                                   si.leveldims(m).height);
        si.minwh[nmip] = 0;  // One past the end, set to 0
        ++nsubimages;
    } while (inp->seek_subimage(nsubimages, 0));
    OIIO_DASSERT((size_t)nsubimages == m_subimages.size());

    m_total_imagesize_ondisk = imagesize_t(Filesystem::file_size(m_filename));

    thread_info->m_stats.files_totalsize -= old_total_imagesize;
    thread_info->m_stats.files_totalsize += m_total_imagesize;
    thread_info->m_stats.files_totalsize_ondisk -= old_total_imagesize_ondisk;
    thread_info->m_stats.files_totalsize_ondisk += m_total_imagesize_ondisk;

    init_from_spec();  // Fill in the rest of the fields
    set_imageinput(inp);

    // reduce memory usage if possible
    // if (m_pool_specs.size() > 1024)
    m_pool_specs.shrink_to_fit();
    // if (m_pool_dims.size() > 1024)
    m_pool_dims.shrink_to_fit();

    return inp;
}



void
ImageCacheFile::init_from_spec()
{
    const SubimageInfo& si(this->subimageinfo(0));
    const ImageSpec& spec = si.spec();
    const ImageDims& dims = si.leveldims(0);
    const ParamValue* p;

    if ((p = spec.find_attribute("wrapmodes", TypeDesc::STRING))) {
        const char* wrapmodes = *(const char**)p->data();
        TextureOpt::parse_wrapmodes(wrapmodes, m_swrap, m_twrap);
        m_rwrap = m_swrap;
        // FIXME(volume) -- rwrap
    }

    m_y_up          = m_imagecache.latlong_y_up_default();
    m_sample_border = false;
    if (m_texformat == TexFormatLatLongEnv
        || m_texformat == TexFormatCubeFaceEnv
        || m_texformat == TexFormatCubeFaceShadow) {
        if (spec.get_string_attribute("oiio:updirection") == "y")
            m_y_up = true;
        else if (spec.get_string_attribute("oiio:updirection") == "z")
            m_y_up = false;
        if (spec.get_int_attribute("oiio:sampleborder") != 0)
            m_sample_border = true;
    }

    if (m_texformat == TexFormatCubeFaceEnv
        || m_texformat == TexFormatCubeFaceShadow) {
        int w = std::max(dims.full_width, dims.tile_width);
        int h = std::max(dims.full_height, dims.tile_height);
        if (dims.width == 3 * w && dims.height == 2 * h)
            m_envlayout = LayoutCubeThreeByTwo;
        else if (dims.width == w && dims.height == 6 * h)
            m_envlayout = LayoutCubeOneBySix;
        else
            m_envlayout = LayoutTexture;
    }

    // Squash some problematic texture metadata if we suspect it's wrong
    pvt::check_texture_metadata_sanity(this->spec(0));

    // See if there's a SHA-1 hash in the image description
    string_view fing = spec.get_string_attribute("oiio:SHA-1");
    if (fing.length())
        m_fingerprint = ustring(fing);

    m_mod_time = Filesystem::last_write_time(m_filename);

    // Set all mipmap level read counts to zero
    int maxmip = 1;
    for (int s = 0, nsubimages = subimages(); s < nsubimages; ++s)
        maxmip = std::max(maxmip, miplevels(s));
    m_mipreadcount.clear();
    m_mipreadcount.resize(maxmip, 0);

    OIIO_DASSERT(!m_broken);
    m_validspec = true;
}



bool
ImageCacheFile::read_tile(ImageCachePerThreadInfo* thread_info,
                          const TileID& id, void* data)
{
    OIIO_DASSERT(id.chend() > id.chbegin());

    // Mark if we ever use a mip level that's not the first
    int miplevel = id.miplevel();
    if (miplevel > 0)
        m_mipused = true;
    // count how many times this mipmap level was read
    m_mipreadcount[miplevel]++;

    int subimage = id.subimage();
    const SubimageInfo& si(subimageinfo(subimage));
    const ImageDims& dims(si.leveldims(miplevel));

    // Special case for un-MIP-mapped
    if (si.unmipped && miplevel != 0)
        return read_unmipped(thread_info, id, data);

    std::shared_ptr<ImageInput> inp = open(thread_info);
    if (!inp)
        return false;

    // Special case for untiled images -- need to do tile emulation
    if (si.untiled)
        return read_untiled(thread_info, inp.get(), id, data);

    // Ordinary tiled
    int x           = id.x();
    int y           = id.y();
    int z           = id.z();
    int chbegin     = id.chbegin();
    int chend       = id.chend();
    TypeDesc format = id.file().datatype(subimage);

    bool ok = true;
    for (int tries = 0; tries <= imagecache().failure_retries(); ++tries) {
        ok = inp->read_tiles(subimage, miplevel, x, x + dims.tile_width, y,
                             y + dims.tile_height, z, z + dims.tile_depth,
                             chbegin, chend, format, data);
        if (ok) {
            if (tries)  // succeeded, but only after a failure!
                ++thread_info->m_stats.tile_retry_success;
            (void)inp->geterror();  // Eat the errors
            break;
        }
        if (tries < imagecache().failure_retries()) {
            // We failed, but will wait a bit and try again.
            Sysutil::usleep(1000 * 100);  // 100 ms
            // TODO: should we attempt to close and re-open the file?
        }
    }
    if (!ok) {
        m_broken        = true;
        std::string err = inp->geterror();
        if (errors_should_issue()) {
            imagecache().error("{}",
                               err.size() ? err : std::string("unknown error"));
        }
    }

    if (ok) {
        size_t b = si.get_tile_bytes(miplevel);
        thread_info->m_stats.bytes_read += b;
        m_bytesread += b;
        ++m_tilesread;
        if (id.colortransformid() > 0) {
            // print("CONVERT id {} {},{} to cs {}\n", filename(), id.x(), id.y(),
            //       id.colortransformid());
            ImageSpec tilespec(dims.tile_width, dims.tile_height,
                               dims.nchannels, format);
            ImageBuf wrapper(tilespec, make_cspan((const std::byte*)data,
                                                  tilespec.image_bytes()));
            ImageBufAlgo::colorconvert(
                wrapper, wrapper,
                ColorConfig::default_colorconfig().getColorSpaceNameByIndex(
                    (id.colortransformid() >> 16) - 1),
                m_imagecache.colorspace(), true, string_view(), string_view(),
                nullptr, ROI(), 1);
        }
    }
    return ok;
}



bool
ImageCacheFile::read_unmipped(ImageCachePerThreadInfo* thread_info,
                              const TileID& id, void* data)
{
    // We need a tile from an unmipmapped file, and it doesn't really
    // exist.  So generate it out of thin air by interpolating pixels
    // from the next higher-res level.  Of course, that may also not
    // exist, but it will be generated recursively, since we call
    // imagecache->get_pixels(), and it will ask for other tiles, which
    // will again call read_unmipped... eventually it will hit a subimage 0
    // tile that actually exists.

    // N.B. No need to lock the mutex, since this is only called
    // from read_tile, which already holds the lock.

    int subimage = id.subimage();
    int miplevel = id.miplevel();
    int x        = id.x();
    int y        = id.y();
    // int z        = id.z();
    int chbegin     = id.chbegin();
    int chend       = id.chend();
    TypeDesc format = id.file().datatype(id.subimage());

    // Figure out the size and strides for a single tile, make an ImageBuf
    // to hold it temporarily.
    const SubimageInfo& si(this->subimageinfo(subimage));
    const ImageDims& dims(si.leveldims(miplevel));
    int tw = dims.tile_width;
    int th = dims.tile_height;
    OIIO_DASSERT(chend > chbegin);
    int nchans = chend - chbegin;
    ImageSpec lospec(tw, th, nchans, TypeDesc::FLOAT);
    ImageBuf lores(lospec);

    // Figure out the range of texels we need for this tile
    x -= dims.x;
    y -= dims.y;
    // z -= dims.z;
    int x0 = x - (x % dims.tile_width);
    int x1 = std::min(x0 + dims.tile_width - 1, dims.full_width - 1);
    int y0 = y - (y % dims.tile_height);
    int y1 = std::min(y0 + dims.tile_height - 1, dims.full_height - 1);
    //    int z0 = z - (z % dims.tile_depth);
    //    int z1 = std::min (z0+dims.tile_depth-1, dims.full_depth-1);

    // Save the contents of the per-thread microcache.  This is because
    // a caller several levels up may be retaining a reference to
    // thread_info->tile and expecting it not to suddenly point to a
    // different tile id!  It's a very reasonable assumption that if you
    // ask to read the last-found tile, it will still be the last-found
    // tile after the pixels are read.  Well, except that below our call
    // to get_pixels may recursively trigger more tiles to be read, and
    // totally change the microcache.  Simple solution: save & restore it.
    ImageCacheTileRef oldtile     = thread_info->tile;
    ImageCacheTileRef oldlasttile = thread_info->lasttile;

    // Auto-mipping will totally thrash the cache if the user unwisely
    // sets it to be too small compared to the image file that needs to
    // automipped.  So we simply override bad decisions by adjusting the
    // cache size to be a minimum of twice as big as any image we automip.
    imagecache().set_min_cache_size(2 * (long long)si.spec().image_bytes());

    // Texel by texel, generate the values by interpolating filtered
    // lookups form the next finer subimage.
    const ImageDims& prev(si.leveldims(miplevel - 1));
    span<float> bilerppels = OIIO_ALLOCA_SPAN(float, 4 * nchans);
    span<float> resultpel  = OIIO_ALLOCA_SPAN(float, nchans);
    bool ok                = true;
    // FIXME(volume) -- loop over z, too
    for (int j = y0; j <= y1; ++j) {
        float yf = (j + 0.5f) / dims.full_height;
        int ylow;
        float yfrac = floorfrac(yf * prev.full_height - 0.5, &ylow);
        for (int i = x0; i <= x1; ++i) {
            float xf = (i + 0.5f) / dims.full_width;
            int xlow;
            float xfrac = floorfrac(xf * prev.full_width - 0.5, &xlow);
            ok &= imagecache().get_pixels(this, thread_info, subimage,
                                          miplevel - 1, xlow, xlow + 2, ylow,
                                          ylow + 2, 0, 1, chbegin, chend,
                                          TypeFloat, bilerppels.data());
            bilerp(bilerppels.data() + 0, bilerppels.data() + nchans,
                   bilerppels.data() + 2 * nchans,
                   bilerppels.data() + 3 * nchans, xfrac, yfrac, nchans,
                   resultpel.data());
            lores.setpixel(i - x0, j - y0, resultpel);
        }
    }

    // Now convert and copy those values out to the caller's buffer
    lores.get_pixels(ROI(0, tw, 0, th, 0, 1, 0, nchans), format,
                     make_span((std::byte*)data,
                               size_t(tw * th * nchans) * format.size()));

    // Restore the microcache to the way it was before.
    thread_info->tile     = oldtile;
    thread_info->lasttile = oldlasttile;

    return ok;
}



// Helper routine for read_tile that handles the rare (but tricky) case
// of reading a "tile" from a file that's scanline-oriented.
bool
ImageCacheFile::read_untiled(ImageCachePerThreadInfo* thread_info,
                             ImageInput* inp, const TileID& id, void* data)
{
    const int subimage         = id.subimage();
    const int miplevel         = id.miplevel();
    const int x                = id.x();
    const int y                = id.y();
    const int z                = id.z();
    const int chbegin          = id.chbegin();
    const int chend            = id.chend();
    const TypeDesc format      = id.file().datatype(id.subimage());
    const int colortransformid = id.colortransformid();

    // Strides for a single tile
    const SubimageInfo& si(this->subimageinfo(subimage));
    const ImageDims& dims(si.leveldims(miplevel));
    int tw = dims.tile_width;
    int th = dims.tile_height;
    OIIO_DASSERT(chend > chbegin);
    int nchans       = chend - chbegin;
    stride_t xstride = AutoStride, ystride = AutoStride, zstride = AutoStride;
    ImageSpec::auto_stride(xstride, ystride, zstride, format, nchans, tw, th);

    bool ok = true;
    if (subimageinfo(subimage).autotiled) {
        // Auto-tile is on, with a tile size that isn't the whole image.
        // We're only being asked for one tile, but since it's a
        // scanline image, we are forced to read (at the very least) a
        // whole row of tiles.  So we add all those tiles to the cache,
        // if not already present, on the assumption that it's highly
        // likely that they will also soon be requested.
        // FIXME -- I don't think this works properly for 3D images
        size_t pixelsize = size_t(nchans * format.size());
        // Because of the way we copy below, we need to allocate the
        // buffer to be an even multiple of the tile width, so round up.
        stride_t scanlinesize = tw * ((dims.width + tw - 1) / tw);
        scanlinesize *= pixelsize;
        std::unique_ptr<char[]> buf(
            new char[scanlinesize * th]);  // a whole tile-row size
        int yy = y - dims.y;               // counting from top scanline
        // [y0,y1] is the range of scanlines to read for a tile-row
        int y0 = yy - (yy % th);
        int y1 = std::min(y0 + th - 1, dims.height - 1);
        y0 += dims.y;
        y1 += dims.y;
        // Read the whole tile-row worth of scanlines
        ok = inp->read_scanlines(subimage, miplevel, y0, y1 + 1, z, chbegin,
                                 chend, format, (void*)&buf[0], pixelsize,
                                 scanlinesize);
        if (!ok) {
            std::string err = inp->geterror();
            if (!err.empty() && errors_should_issue())
                imagecache().error("{}", err);
        }
        size_t b = (y1 - y0 + 1) * si.get_scanline_bytes(miplevel);
        thread_info->m_stats.bytes_read += b;
        m_bytesread += b;
        ++m_tilesread;

        // For all tiles in the tile-row, enter them into the cache if not
        // already there.  Special case for the tile we're actually being
        // asked for -- save it in 'data' rather than adding a tile.
        int xx = x - dims.x;      // counting from left row
        int x0 = xx - (xx % tw);  // start of the tile we are retrieving
        for (int i = 0; i < dims.width; i += tw) {
            if (i == xx) {
                // This is the tile we've been asked for
                convert_image(nchans, tw, th, 1, &buf[x0 * pixelsize], format,
                              pixelsize, scanlinesize, scanlinesize * th, data,
                              format, xstride, ystride, zstride);
            } else {
                // Not the tile we asked for, but it's in the same
                // tile-row, so let's put it in the cache anyway so
                // it'll be there when asked for.
                TileID id(*this, subimage, miplevel, i + dims.x, y0, z, chbegin,
                          chend, colortransformid);
                if (!imagecache().tile_in_cache(id, thread_info)) {
                    ImageCacheTileRef tile;
                    tile = new ImageCacheTile(id, &buf[i * pixelsize], format,
                                              pixelsize, scanlinesize,
                                              scanlinesize * th);
                    ok &= tile->valid();
                    ok &= imagecache().add_tile_to_cache(tile, thread_info);
                }
            }
        }
    } else {
        // No auto-tile -- the tile is the whole image
        ok = inp->read_image(subimage, miplevel, chbegin, chend, format, data,
                             xstride, ystride, zstride);
        if (!ok) {
            std::string err = inp->geterror();
            if (!err.empty() && errors_should_issue())
                imagecache().error("{}", err);
        }
        size_t b = si.get_image_bytes(miplevel);
        thread_info->m_stats.bytes_read += b;
        m_bytesread += b;
        ++m_tilesread;
        // If we read the whole image (and there are no other subimages),
        // presumably we're done, so release the file handle.
        if (subimages() == 1)
            close();
        // FIXME: ^^^ is that a good idea or not?
    }

    return ok;
}



void
ImageCacheFile::close()
{
    // Clearing the m_input will close and free it when no other threads
    // are still hanging onto it.
    std::shared_ptr<ImageInput> empty;
    set_imageinput(empty);
}



void
ImageCacheFile::release()
{
    using namespace std::chrono_literals;
    Timer input_mutex_timer;
    bool locked = m_input_mutex.try_lock_for(100ms);
    m_mutex_wait_time += input_mutex_timer();
    if (!locked) {
        // Oh boy, somebody is holding this lock. Rather than sit here even
        // longer (or even worse -- deadlock), just punt and return. The worst
        // that can happen is that a file that should have had its handle
        // reclaimed might get held open a little longer. If there's lots of
        // pressure on the cache, it'll get freed next time around.
        return;
    }
    if (m_used)
        m_used = false;
    else if (m_allow_release)
        close();
    m_input_mutex.unlock();
}



void
ImageCacheFile::invalidate()
{
    Timer input_mutex_timer;
    recursive_timed_lock_guard guard(m_input_mutex);
    m_mutex_wait_time += input_mutex_timer();
    close();
    invalidate_spec();
    mark_not_broken();
    m_fingerprint.clear();
    duplicate(NULL);

    m_filename = m_imagecache.resolve_filename(m_filename_original.string());

    // Eat any errors that occurred in the open/close
    while (!imagecache().geterror().empty())
        ;
    m_errors_issued = 0;  // start error count fresh
}



bool
ImageCacheFile::get_average_color(float* avg, int subimage, int chbegin,
                                  int chend)
{
    if (subimage < 0 || subimage > subimages())
        return false;  // invalid subimage
    SubimageInfo& si(m_subimages[subimage]);

    if (!si.has_average_color) {
        // try to figure it out by grabbing the single pixel at the 1x1
        // MIP level.
        int nlevels = (int)si.levels.size();
        const ImageDims& dims(si.leveldims(nlevels - 1));
        if (dims.width != 1 || dims.height != 1 || dims.depth != 1)
            return false;  // no hope, there's no 1x1 MIP level to sample
        spin_lock lock(si.average_color_mutex);
        if (!si.has_average_color) {
            si.average_color.resize(dims.nchannels);
            bool ok = m_imagecache.get_pixels(this, NULL, subimage, nlevels - 1,
                                              dims.x, dims.x + 1, dims.y,
                                              dims.y + 1, dims.z, dims.z + 1, 0,
                                              dims.nchannels, TypeFloat,
                                              &si.average_color[0]);
            si.has_average_color = ok;
        }
    }

    if (si.has_average_color) {
        const ImageSpec& spec(si.spec());
        for (int i = 0, c = chbegin; c < chend; ++i, ++c)
            avg[i] = (c < spec.nchannels) ? si.average_color[c] : 0.0f;
        return true;
    }

    return false;
}



int
ImageCacheFile::errors_should_issue() const
{
    return (++m_errors_issued <= imagecache().max_errors_per_file());
}



void
ImageCacheFile::mark_not_broken()
{
    m_broken = false;
    m_broken_message.clear();
}



void
ImageCacheFile::mark_broken(string_view error)
{
    m_broken = true;
    if (!error.size())
        error = string_view("unknown error");
    m_broken_message = error;
    imagecache().error("{}", error);
    invalidate_spec();
}



size_t
ImageCacheFile::heapsize() const
{
    size_t size = pvt::heapsize(m_subimages);
    size += pvt::heapsize(m_pool_specs);
    size += pvt::heapsize(m_pool_dims);
    size += pvt::heapsize(m_configspec);
    size += pvt::heapsize(m_input);
    size += pvt::heapsize(m_mipreadcount);
    size += pvt::heapsize(m_udim_lookup);
    return size;
}



ImageCacheFile*
ImageCacheImpl::find_file(ustring filename,
                          ImageCachePerThreadInfo* thread_info,
                          ImageInput::Creator creator, const ImageSpec* config,
                          bool replace)
{
    // Debugging aid: attribute "substitute_image" forces all image
    // references to be to one named file.
    if (!m_substitute_image.empty())
        filename = m_substitute_image;

    // Shortcut - check the per-thread microcache before grabbing a more
    // expensive lock on the shared file cache.
    ImageCacheFile* tf = replace ? nullptr : thread_info->find_file(filename);

    // Make sure the ImageCacheFile entry exists and is in the
    // file cache.  For this part, we need to lock the file cache.
    bool newfile = false;
    if (!tf) {  // was not found in microcache
#if IMAGECACHE_TIME_STATS
        Timer timer;
#endif
        size_t bin                  = m_files.lock_bin(filename);
        FilenameMap::iterator found = m_files.find(filename, false);
        if (found) {
            tf = found->second.get();
        } else {
            // No such entry in the file cache.  Add it, but don't open yet.
            tf = new ImageCacheFile(*this, thread_info, filename, creator,
                                    config);
            m_files.insert(filename, tf, false);
            newfile = true;
        }
        m_files.unlock_bin(bin);
        if (replace && found) {
            invalidate(filename, true);
            tf->reset(creator, config);
        }

        if (newfile) {
            // We don't need to check_max_files here, because open() already
            // does it, and we're only trying to limit the number of open
            // files, not the number of entries in the cache.
            // check_max_files(thread_info);
            if (!tf->duplicate())
                ++thread_info->m_stats.unique_files;
        }
        thread_info->remember_filename(filename, tf);  // add to the microcache
#if IMAGECACHE_TIME_STATS
        thread_info->m_stats.find_file_time += timer();
#endif
    }

    // Ensure that it's open and do other important housekeeping.
    //    tf = verify_file (tf, thread_info, header_only);
    return tf;
}



ImageCacheFile*
ImageCacheImpl::verify_file(ImageCacheFile* tf,
                            ImageCachePerThreadInfo* thread_info,
                            bool header_only)
{
    if (!tf)
        return NULL;

    if (tf->is_udim()) {
        // Can't really open a UDIM-like virtual file
        return tf;
    }

    // Open the file if it's never been opened before.
    // No need to have the file cache locked for this, though we lock
    // the tf->m_input_mutex if we need to open it.
    if (!tf->validspec()) {
        Timer timer;
        if (!thread_info)
            thread_info = get_perthread_info();
        Timer input_mutex_timer;
        recursive_timed_lock_guard guard(tf->m_input_mutex);
        tf->m_mutex_wait_time += input_mutex_timer();
        if (!tf->validspec()) {
            tf->open(thread_info);
            OIIO_DASSERT(tf->m_broken || tf->validspec());
            double createtime = timer();
            ImageCacheStatistics& stats(thread_info->m_stats);
            stats.fileio_time += createtime;
            stats.fileopen_time += createtime;
            tf->iotime() += createtime;

            // What if we've opened another file, with a different name,
            // but the SAME pixels?  It can happen!  Bad user, bad!  But
            // let's save them from their own foolishness.
            if (!tf->fingerprint().empty() && m_deduplicate) {
                // std::cerr << filename << " hash=" << tf->fingerprint() << "\n";
                ImageCacheFile* dup = find_fingerprint(tf->fingerprint(), tf);
                if (dup != tf) {
                    // Already in fingerprints -- mark this one as a
                    // duplicate, but ONLY if we don't have other
                    // reasons not to consider them true duplicates (the
                    // fingerprint only considers source image pixel values,
                    // and of course only if they are the same "shape".
                    // FIXME -- be sure to add extra tests
                    // here if more metadata have significance later!
                    bool match = (tf->subimages() == dup->subimages());
                    const SubimageInfo& tfsi(tf->subimageinfo(0));
                    const ImageDims& tfdims(tfsi.leveldims(0));
                    const SubimageInfo& dupsi(dup->subimageinfo(0));
                    const ImageDims& dupdims(dupsi.leveldims(0));
                    match &= (tfdims.width == dupdims.width
                              && tfdims.height == dupdims.height
                              && tfdims.depth == dupdims.depth
                              && tfdims.nchannels == dupdims.nchannels
                              && tf->subimages() == dup->subimages()
                              && tf->miplevels(0) == dup->miplevels(0)
                              && tf->m_swrap == dup->m_swrap
                              && tf->m_twrap == dup->m_twrap
                              && tf->m_rwrap == dup->m_rwrap
                              && tf->m_envlayout == dup->m_envlayout
                              && tf->m_y_up == dup->m_y_up
                              && tf->m_sample_border == dup->m_sample_border);
                    for (int s = 0, e = tf->subimages(); match && s < e; ++s) {
                        match &= (tf->datatype(s) == dup->datatype(s));
                    }
                    if (match) {
                        tf->duplicate(dup);
                        tf->close();
                        // std::cerr << "  duplicates "
                        //   << fingerfound->second.get()->filename() << "\n";
                    }
                }
            }
#if IMAGECACHE_TIME_STATS
            stats.find_file_time += timer() - createtime;
#endif
        }
    }

    if (!header_only) {
        // if this is a duplicate texture, switch to the canonical copy
        // N.B. If looking up header info (i.e., get_image_info, rather
        // than getting pixels, use the original not the duplicate, since
        // metadata may differ even if pixels are identical).
        if (tf->duplicate())
            tf = tf->duplicate();
        tf->use();  // Mark it as recently used
    }
    return tf;
}



ImageCacheFile*
ImageCacheImpl::find_fingerprint(ustring finger, ImageCacheFile* file)
{
    spin_lock lock(m_fingerprints_mutex);
    // Insert if missing, otherwise return old value
    return m_fingerprints.emplace(finger, file).first->second.get();
}



void
ImageCacheImpl::clear_fingerprints()
{
    spin_lock lock(m_fingerprints_mutex);
    m_fingerprints.clear();
}



void
ImageCacheImpl::check_max_files(ImageCachePerThreadInfo* /*thread_info*/)
{
#if 0
    if (! (m_stat_open_files_created % 16) || m_stat_open_files_current >= m_max_open_files) {
        std::cerr << "open files " << m_stat_open_files_current << ", max = " << m_max_open_files << "\n";
    std::cout << "    ImageInputs : " << m_stat_open_files_created << " created, " << m_stat_open_files_current << " current, " << m_stat_open_files_peak << " peak\n";
    }
#endif

    int open_files = m_stat_open_files_current;
    if (open_files < m_max_open_files) {
        // Early out if we aren't exceeding the open file handle limit
        return;
    }
    if (!m_max_open_files_strict || open_files < m_max_open_files + 16) {
        // If we're only exceeding the open files limit by a little bit (or by
        // any amount if we aren't trying to be strict about), try to grab the
        // file_sweep_mutex lock. If we get it, we do the cleanup. But if
        // somebody else holds it, just return rather than blocking, leaving
        // the handle limit enforcement to whomever is already in this
        // function or gets here next. No need for two threads to do it at
        // once or to block. If this means we may ephemerally be over the
        // handle limit, so be it.
        if (!m_file_sweep_mutex.try_lock())
            return;
    } else {
        // But if we're already quite a bit over the limit, we don't want to
        // keep deferring, because if there are a large number of threads and
        // a low max_open_files, we may go significantly over the limit if we
        // are too quick to shirk our duty. So grab the lock unconditionally
        // and do the work to close some of those handles.
        m_file_sweep_mutex.lock();
    }

    // Now, what we want to do is have a "clock hand" that sweeps across
    // the cache, releasing files that haven't been used for a long
    // time.  Because of multi-thread, rather than keep an iterator
    // around for this (which could be invalidated since the last time
    // we used it), we just remember the filename of the next file to
    // check, then look it up fresh.  That is m_file_sweep_name.

    // Get a (locked) iterator for the next tile to be examined.
    FilenameMap::iterator sweep;
    if (!m_file_sweep_name.empty()) {
        // We saved the sweep_id. Find the iterator corresponding to it.
        sweep = m_files.find(m_file_sweep_name);
        // Note: if the sweep_id is no longer in the table, sweep will be an
        // empty iterator. That's ok, it will be fixed early in the main
        // loop below.
    }


    // Loop while we still have too many files open.  Also, be careful
    // of looping for too long, exit the loop if we just keep spinning
    // uncontrollably.
    int full_loops = 0;
    while (m_stat_open_files_current >= m_max_open_files && full_loops <= 100) {
        // If we have fallen off the end of the cache, loop back to the
        // beginning and increment our full_loops count.
        if (!sweep) {
            sweep = m_files.begin();
            ++full_loops;
        }
        // If we're STILL at the end, it must be that somehow the entire
        // cache is empty.  So just declare ourselves done.
        if (!sweep)
            break;
        OIIO_DASSERT(sweep->second);
        sweep->second->release();  // May reduce open files
        ++sweep;
        // Note: This loop is a lot less complicated than the one in
        // ImageCacheImpl::check_max_mem. That's because for the file
        // cache, we just need to release old ones, we don't actually
        // erase entries from the cache, so there is no tricky lock/unlock,
        // erasure, or invalidation of iterators.
    }

    // OK, by this point we have either closed enough files to be below
    // the limit again, or the cache is empty, or we've looped over the
    // cache too many times and are giving up.

    // Now we must save the filename for next time.  Just set it to an
    // empty string if we don't have a valid iterator at this point.
    m_file_sweep_name = (sweep ? sweep->first : ustring());
    m_file_sweep_mutex.unlock();

    // N.B. As we exit, the iterators will go out of scope and we will
    // retain no locks on the cache.
}



void
ImageCacheImpl::set_min_cache_size(long long newsize)
{
    atomic_max(m_max_memory_bytes, newsize);
}



ImageCacheTile::ImageCacheTile(const TileID& id)
    : m_id(id)
    , m_valid(true)
{
    id.file().imagecache().incr_tiles(0);  // mem counted separately in read
}



ImageCacheTile::ImageCacheTile(const TileID& id, const void* pels,
                               TypeDesc format, stride_t xstride,
                               stride_t ystride, stride_t zstride, bool copy)
    : m_id(id)
{
    ImageCacheFile& file(m_id.file());
    const SubimageInfo& si(file.subimageinfo(m_id.subimage()));
    const ImageDims& dims(si.leveldims(m_id.miplevel()));
    m_channelsize = file.datatype(id.subimage()).size();
    m_pixelsize   = id.nchannels() * m_channelsize;
    m_tile_width  = dims.tile_width;
    if (copy) {
        size_t size = memsize_needed();
        OIIO_ASSERT_MSG(size > 0 && memsize() == 0,
                        "size was %llu, memsize = %llu",
                        (unsigned long long)size,
                        (unsigned long long)memsize());
        m_pixels_size = size;
        m_pixels.reset(new char[m_pixels_size]);
        m_valid
            = convert_image(id.nchannels(), dims.tile_width, dims.tile_height,
                            dims.tile_depth, pels, format, xstride, ystride,
                            zstride, &m_pixels[0], file.datatype(id.subimage()),
                            m_pixelsize, m_pixelsize * dims.tile_width,
                            m_pixelsize * dims.tile_width * dims.tile_height);
    } else {
        m_nofree      = true;  // Don't free the pointer!
        m_pixels_size = 0;
        m_pixels.reset((char*)pels);
        m_valid = true;
    }
    id.file().imagecache().incr_tiles(m_pixels_size);
    m_pixels_ready = true;  // Caller sent us the pixels, no read necessary
    // FIXME -- for shadow, fill in mindepth, maxdepth
}



ImageCacheTile::~ImageCacheTile()
{
    m_id.file().imagecache().decr_tiles(memsize());
    if (m_nofree)
        m_pixels.release();  // release without freeing
}



size_t
ImageCacheTile::memsize_needed() const
{
    const SubimageInfo& si(file().subimageinfo(m_id.subimage()));
    size_t s = si.get_tile_pixels(m_id.miplevel()) * pixelsize();
    // N.B. Round up so we can use a SIMD fetch for the last pixel and
    // channel without running off the end.
    s += OIIO_SIMD_MAX_SIZE_BYTES;
    return s;
}



bool
ImageCacheTile::read(ImageCachePerThreadInfo* thread_info)
{
    ImageCacheFile& file(m_id.file());
    m_channelsize = file.datatype(id().subimage()).size();
    m_pixelsize   = m_id.nchannels() * m_channelsize;
    size_t size   = memsize_needed();
    OIIO_ASSERT(memsize() == 0 && size > OIIO_SIMD_MAX_SIZE_BYTES);
    m_pixels.reset(new char[m_pixels_size = size]);
    // Clear the end pad values so there aren't NaNs sucked up by simd loads
    memset(m_pixels.get() + size - OIIO_SIMD_MAX_SIZE_BYTES, 0,
           OIIO_SIMD_MAX_SIZE_BYTES);
    m_valid = file.read_tile(thread_info, m_id, &m_pixels[0]);
    file.imagecache().incr_mem(size);
    if (m_valid) {
        SubimageInfo& si(file.subimageinfo(m_id.subimage()));
        LevelInfo& lev(si.levelinfo(m_id.miplevel()));
        const ImageDims& dims(si.leveldims(m_id.miplevel()));
        m_tile_width = dims.tile_width;
        OIIO_DASSERT(m_tile_width > 0);
        int whichtile = ((m_id.x() - dims.x) / dims.tile_width)
                        + ((m_id.y() - dims.y) / dims.tile_height) * lev.nxtiles
                        + ((m_id.z() - dims.z) / dims.tile_depth)
                              * (lev.nxtiles * lev.nytiles);
        int index       = whichtile / 64;
        int64_t bitmask = int64_t(1ULL << (whichtile & 63));
        int64_t oldval  = lev.tiles_read[index].fetch_or(bitmask);
        if (oldval & bitmask)  // Was it previously read?
            file.register_redundant_tile(si.get_tile_bytes(m_id.miplevel()));
    } else {
        // (! m_valid)
        m_used = false;  // Don't let it hold mem if invalid
        if (file.mod_time() != Filesystem::last_write_time(file.filename()))
            file.imagecache().error(
                "File \"{}\" was modified after being opened by OIIO",
                file.filename());
        file.imagecache().error(
            "Error reading from \"{}\" (subimg={}, mip={}, tile@{},{},{})",
            file.filename(), m_id.subimage(), m_id.miplevel(), m_id.x(),
            m_id.y(), m_id.z());
#if 0
        std::cerr << "(1) error reading tile " << m_id.x() << ' ' << m_id.y()
                  << ' ' << m_id.z()
                  << " subimg=" << m_id.subimage()
                  << " mip=" << m_id.miplevel()
                  << " from " << file.filename() << "\n";
#endif
    }
    m_pixels_ready = true;
    // FIXME -- for shadow, fill in mindepth, maxdepth
    return m_valid;
}



void
ImageCacheTile::wait_pixels_ready() const
{
    atomic_backoff backoff;
    while (!m_pixels_ready) {
        backoff();
    }
}



const void*
ImageCacheTile::data(int x, int y, int z, int c) const
{
    const SubimageInfo& si(file().subimageinfo(m_id.subimage()));
    const ImageDims& dims(si.leveldims(m_id.miplevel()));
    size_t w = dims.tile_width;
    size_t h = dims.tile_height;
    size_t d = dims.tile_depth;
    OIIO_DASSERT(d >= 1);
    x -= m_id.x();
    y -= m_id.y();
    z -= m_id.z();
    if (x < 0 || x >= (int)w || y < 0 || y >= (int)h || z < 0 || z >= (int)d
        || c < m_id.chbegin() || c > m_id.chend())
        return NULL;
    size_t offset = ((z * h + y) * w + x) * pixelsize()
                    + (c - m_id.chbegin()) * channelsize();
    return (const void*)&m_pixels[offset];
}



ImageCacheImpl::ImageCacheImpl()
{
    imagecache_id = imagecache_next_id.fetch_add(1);
    init();
}



void
ImageCacheImpl::set_max_open_files(int max_open_files)
{
    // Clamp to somewhat less than the maximum number of files allowed
    // by the system.
    int m = int(std::min(Sysutil::max_open_files(),
                         size_t(std::numeric_limits<int>::max())));
    m     = std::max(10, m - 5 * int(Sysutil::hardware_concurrency()));
    m_max_open_files = std::min(max_open_files, m);
    // std::cout << "clamped max_open_files = " << m_max_open_files << "\n";
}



void
ImageCacheImpl::init()
{
    set_max_open_files(100);
    m_max_memory_bytes     = 1024LL * 1024 * 1024;  // 1 GB default cache size
    m_autotile             = 0;
    m_autoscanline         = false;
    m_automip              = false;
    m_forcefloat           = false;
    m_accept_untiled       = true;
    m_accept_unmipped      = true;
    m_deduplicate          = true;
    m_unassociatedalpha    = false;
    m_failure_retries      = 0;
    m_latlong_y_up_default = true;
    m_Mw2c.makeIdentity();
    m_colorspace              = ustring("scene_linear");
    m_mem_used                = 0;
    m_statslevel              = 0;
    m_max_errors_per_file     = 100;
    m_stat_tiles_created      = 0;
    m_stat_tiles_current      = 0;
    m_stat_tiles_peak         = 0;
    m_stat_open_files_created = 0;
    m_stat_open_files_current = 0;
    m_stat_open_files_peak    = 0;
    m_max_open_files_strict   = false;

    // Allow environment variable to override default options
    const char* options = getenv("OPENIMAGEIO_IMAGECACHE_OPTIONS");
    if (options)
        attribute("options", options);
}



ImageCacheImpl::~ImageCacheImpl()
{
    printstats();
    // All the per_thread_infos get destroyed here, regardless of if they were created implicitly
    // or manually by the caller
    {
        spin_lock lock(m_perthread_info_mutex);
        m_all_perthread_info.clear();
    }
    // Erase any leftover errors from this thread
    // TODO: can we clear other threads' errors?
    // TODO: potentially unsafe due to the static destruction order fiasco
    // imcache_error_messages.erase(imagecache_id);
}



void
ImageCacheImpl::mergestats(ImageCacheStatistics& stats) const
{
    stats.init();
    spin_lock lock(m_perthread_info_mutex);
    for (auto& p : m_all_perthread_info)
        if (p)
            stats.merge(p->m_stats);
}



std::string
ImageCacheImpl::onefile_stat_line(const ImageCacheFileRef& file, int i,
                                  bool includestats) const
{
    using Strutil::print;

    // FIXME -- make meaningful stat printouts for multi-image textures
    std::ostringstream out;
    out.imbue(std::locale::classic());  // Force "C" locale with '.' decimal
    const SubimageInfo& si(file->subimageinfo(0));
    const ImageSpec& spec(si.spec());
    const char* formatcode = "u8";
    switch (spec.format.basetype) {
    case TypeDesc::UINT8: formatcode = "u8 "; break;
    case TypeDesc::INT8: formatcode = "i8 "; break;
    case TypeDesc::UINT16: formatcode = "u16"; break;
    case TypeDesc::INT16: formatcode = "i16"; break;
    case TypeDesc::UINT: formatcode = "u32"; break;
    case TypeDesc::INT: formatcode = "i32"; break;
    case TypeDesc::UINT64: formatcode = "i64"; break;
    case TypeDesc::INT64: formatcode = "u64"; break;
    case TypeDesc::HALF: formatcode = "f16"; break;
    case TypeDesc::FLOAT: formatcode = "f32"; break;
    case TypeDesc::DOUBLE: formatcode = "f64"; break;
    default: break;
    }
    if (i >= 0)
        Strutil::print(out, "{:7} ", i);
    if (includestats) {
        unsigned long long redund_tiles = file->redundant_tiles();
        if (redund_tiles)
            Strutil::print(out, "{:4}  {:7}   {:8.1f}   ({:5} {:6.1f}) {:9}  ",
                  file->timesopened(), file->tilesread(),
                  file->bytesread() / 1024.0 / 1024.0, redund_tiles,
                  file->redundant_bytesread() / 1024.0 / 1024.0,
                  Strutil::timeintervalformat(file->iotime()));
        else
            Strutil::print(out, "{:4}  {:7}   {:8.1f}                  {:9}  ",
                  file->timesopened(), file->tilesread(),
                  file->bytesread() / 1024.0 / 1024.0,
                  Strutil::timeintervalformat(file->iotime()));
    }
    if (file->subimages() > 1)
        Strutil::print(out, "{:3} face x{}.{}", file->subimages(), spec.nchannels,
              formatcode);
    else
        Strutil::print(out, "{:4}x{:4}x{}.{}", spec.width, spec.height, spec.nchannels,
              formatcode);
    Strutil::print(out, "  {} ", file->filename());
    if (file->duplicate()) {
        Strutil::print(out, " DUPLICATES {}", file->duplicate()->filename());
        return out.str();
    }
    for (int s = 0; s < file->subimages(); ++s)
        if (file->subimageinfo(s).untiled) {
            Strutil::print(out, " UNTILED");
            break;
        }
    if (automip()) {
        // FIXME -- we should directly measure whether we ever automipped
        // this file.  This is a little inexact.
        for (int s = 0; s < file->subimages(); ++s)
            if (file->subimageinfo(s).unmipped) {
                Strutil::print(out, " UNMIPPED");
                break;
            }
    }
    if (!file->mipused()) {
        for (int s = 0; s < file->subimages(); ++s)
            if (!file->subimageinfo(s).unmipped) {
                Strutil::print(out, " MIP-UNUSED");
                break;
            }
    }
    if (file->mipreadcount().size() > 1) {
        Strutil::print(out, " MIP-COUNT[");
        int nmip = (int)file->mipreadcount().size();
        for (int c = 0; c < nmip; c++)
            Strutil::print(out, "{}{}", (c ? "," : ""), file->mipreadcount()[c]);
        Strutil::print(out, "]");
    }

    return out.str();
}



std::string
ImageCacheImpl::getstats(int level) const
{
    using Strutil::print;

    // Merge all the threads
    ImageCacheStatistics stats;
    mergestats(stats);

    // Gather file list and statistics
    size_t total_opens = 0, total_tiles = 0;
    size_t total_redundant_tiles      = 0;
    imagesize_t total_bytes           = 0;
    imagesize_t total_redundant_bytes = 0;
    size_t total_untiled = 0, total_unmipped = 0, total_duplicates = 0;
    size_t total_constant              = 0;
    double total_iotime                = 0;
    double total_input_mutex_wait_time = 0;
    std::vector<ImageCacheFileRef> files;
    {
        for (FilenameMap::iterator f = m_files.begin(); f != m_files.end();
             ++f) {
            const ImageCacheFileRef& file(f->second);
            files.push_back(file);
            total_opens += file->timesopened();
            total_tiles += file->tilesread();
            total_redundant_tiles += file->redundant_tiles();
            total_redundant_bytes += file->redundant_bytesread();
            total_bytes += file->bytesread();
            total_iotime += file->iotime();
            total_input_mutex_wait_time += file->m_mutex_wait_time;
            if (file->duplicate()) {
                ++total_duplicates;
                continue;
            }
            bool found_untiled = false, found_unmipped = false;
            bool found_const = true;
            for (int s = 0, send = file->subimages(); s < send; ++s) {
                const ImageCacheFile::SubimageInfo& si(file->subimageinfo(s));
                found_untiled |= si.untiled;
                found_unmipped |= si.unmipped;
                found_const &= si.is_constant_image;
            }
            total_untiled += found_untiled;
            total_unmipped += found_unmipped;
            total_constant += found_const;
        }
    }

    std::ostringstream out;
    out.imbue(std::locale::classic());  // Force "C" locale with '.' decimal
    if (level > 0) {
        out << "OpenImageIO ImageCache statistics (";
        {
            spin_lock guard(shared_image_cache_mutex);
            if ((void*)this == (void*)shared_image_cache.get())
                out << "shared";
            else
                out << (void*)this;
        }
        out << ") ver " << OIIO_VERSION_STRING << "\n";

        std::string opt;
#define BOOLOPT(name) \
    if (m_##name)     \
    opt += #name " "
#define INTOPT(name) opt += Strutil::fmt::format(#name "={} ", m_##name)
#define STROPT(name)     \
    if (m_##name.size()) \
    opt += Strutil::fmt::format(#name "=\"{}\" ", m_##name)
        opt += Strutil::fmt::format("max_memory_MB={:0.1f} ",
                                    m_max_memory_bytes / (1024.0 * 1024.0));
        INTOPT(max_open_files);
        BOOLOPT(max_open_files_strict);
        INTOPT(autotile);
        INTOPT(autoscanline);
        INTOPT(automip);
        INTOPT(forcefloat);
        INTOPT(accept_untiled);
        INTOPT(accept_unmipped);
        INTOPT(deduplicate);
        INTOPT(unassociatedalpha);
        INTOPT(failure_retries);
        opt += Strutil::fmt::format("openexr:core={} ",
                                    OIIO::get_int_attribute("openexr:core"));
#undef BOOLOPT
#undef INTOPT
#undef STROPT
        Strutil::print(out, "  Options:  {}\n", Strutil::wordwrap(opt, 75, 12));

        if (stats.unique_files) {
            Strutil::print(out, "  Images : {} unique\n", stats.unique_files);
            Strutil::print(out, "    ImageInputs : {} created, {} current, {} peak\n",
                  int(m_stat_open_files_created),
                  int(m_stat_open_files_current), int(m_stat_open_files_peak));
            Strutil::print(out,
                  "    Total pixel data size of all images referenced : {}\n",
                  Strutil::memformat(stats.files_totalsize));
            Strutil::print(out,
                  "    Total actual file size of all images referenced : {}\n",
                  Strutil::memformat(stats.files_totalsize_ondisk));
            Strutil::print(out, "    Pixel data read : {}\n",
                  Strutil::memformat(stats.bytes_read));
        } else {
            Strutil::print(out, "  No images opened\n");
        }
        if (stats.find_file_time > 0.001 || level > 2)
            Strutil::print(out, "    Find file time : {}\n",
                  Strutil::timeintervalformat(stats.find_file_time));
        if (stats.fileio_time > 0.001 || level > 2) {
            Strutil::print(out, "    File I/O time : {}",
                  Strutil::timeintervalformat(stats.fileio_time));
            {
                spin_lock lock(m_perthread_info_mutex);
                size_t nthreads = m_all_perthread_info.size();
                if (nthreads > 1 || level > 2) {
                    double perthreadtime = stats.fileio_time
                                           / std::max(size_t(1), nthreads);
                    Strutil::print(out, " ({} average per thread, for {} threads)",
                          Strutil::timeintervalformat(perthreadtime), nthreads);
                }
            }
            Strutil::print(out, "\n");
            Strutil::print(out, "    File open time only : {}\n",
                  Strutil::timeintervalformat(stats.fileopen_time));
        }
        if (stats.file_locking_time > 0.001 || level > 2)
            Strutil::print(out, "    File mutex locking time : {}\n",
                  Strutil::timeintervalformat(stats.file_locking_time));
        if (total_input_mutex_wait_time > 0.001 || level > 2)
            Strutil::print(out, "    ImageInput mutex locking time : {}\n",
                  Strutil::timeintervalformat(total_input_mutex_wait_time));
        if (m_stat_tiles_created > 0 || level > 2) {
            Strutil::print(out, "  Tiles: {} created, {} current, {} peak\n",
                  int(m_stat_tiles_created), int(m_stat_tiles_current),
                  int(m_stat_tiles_peak));
            Strutil::print(out, "    total tile requests : {}\n", stats.find_tile_calls);
            if (stats.find_tile_microcache_misses)
                Strutil::print(out, "    micro-cache misses : {} ({:.1f}%)\n",
                      stats.find_tile_microcache_misses,
                      100.0 * stats.find_tile_microcache_misses
                          / (double)stats.find_tile_calls);
            if (stats.find_tile_cache_misses)
                Strutil::print(out, "    main cache misses : {} ({:.1f}%)\n",
                      stats.find_tile_cache_misses,
                      100.0 * stats.find_tile_cache_misses
                          / (double)stats.find_tile_calls);
            Strutil::print(out, "    redundant reads: {} tiles, {}\n",
                  total_redundant_tiles,
                  Strutil::memformat(total_redundant_bytes));
        }
        Strutil::print(out, "    Peak cache memory : {}\n",
              Strutil::memformat(m_mem_used));
        if (stats.tile_locking_time > 0.001 || level > 2)
            Strutil::print(out, "    Tile mutex locking time : {}\n",
                  Strutil::timeintervalformat(stats.tile_locking_time));
        if (stats.find_tile_time > 0.001 || level > 2)
            Strutil::print(out, "    Find tile time : {}\n",
                  Strutil::timeintervalformat(stats.find_tile_time));
        if (stats.file_retry_success || stats.tile_retry_success)
            Strutil::print(out,
                  "    Failure reads followed by unexplained success:"
                  " {} files, {} tiles\n",
                  stats.file_retry_success, stats.tile_retry_success);

        printImageCacheMemory(out, *this);
    }

    if (level >= 2 && files.size()) {
        Strutil::print(out, "  Image file statistics:\n");
        Strutil::print(out, "        opens   tiles    MB read   --redundant--   "
                   "I/O time  res              File\n");
        std::sort(files.begin(), files.end(), filename_compare);
        for (size_t i = 0; i < files.size(); ++i) {
            const ImageCacheFileRef& file(files[i]);
            OIIO_DASSERT(file);
            if (file->is_udim())
                continue;
            if (file->broken() || file->subimages() == 0) {
                Strutil::print(out, "  {:76}{}\n", "BROKEN", file->filename());
                continue;
            }
            Strutil::print(out, "{}\n", onefile_stat_line(file, i + 1));
        }
        Strutil::print(out, "\n  Tot:  {:4}  {:7}   {:8.1f}   ({:5} {:6.1f}) {:9}\n",
              total_opens, total_tiles, total_bytes / 1024.0 / 1024.0,
              total_redundant_tiles, total_redundant_bytes / 1024.0 / 1024.0,
              Strutil::timeintervalformat(total_iotime));
    }

    // Try to point out hot spots
    if (level > 0) {
        if (total_duplicates || level > 2)
            Strutil::print(out, "  {} were exact duplicates of other images\n",
                  total_duplicates);
        if (total_untiled || (total_unmipped && automip()) || level > 2) {
            Strutil::print(out, "  {} not tiled, {} not MIP-mapped\n", total_untiled,
                  total_unmipped);
#if 0
            if (files.size() >= 50) {
                Strutil::print(out, "  Untiled/unmipped files were:\n");
                for (size_t i = 0;  i < files.size();  ++i) {
                    const ImageCacheFileRef &file (files[i]);
                    if (file->untiled() || (file->unmipped() && automip()))
                        Strutil::print(out, "{}\n", onefile_stat_line (file, -1));
                }
            }
#endif
        }
        if (total_constant || level > 2)
            Strutil::print(out, "  {} {} constant-valued in all pixels\n",
                  total_constant, (total_constant == 1 ? "was" : "were"));
        if (files.size() >= 50 || level > 2) {
            const int topN = 3;
            int nprinted;
            std::sort(files.begin(), files.end(), bytesread_compare);
            Strutil::print(out, "  Top files by bytes read:\n");
            nprinted = 0;
            for (const ImageCacheFileRef& file : files) {
                if (file->broken() || !file->validspec())
                    continue;
                if (nprinted++ >= topN)
                    break;
                Strutil::print(out, "    {}   {:6.1f} MB ({:4.1f}%)  {}\n", nprinted,
                      file->bytesread() / 1024.0 / 1024.0,
                      100.0 * file->bytesread() / (double)total_bytes,
                      onefile_stat_line(file, -1, false));
            }
            std::sort(files.begin(), files.end(), iotime_compare);
            Strutil::print(out, "  Top files by I/O time:\n");
            nprinted = 0;
            for (const ImageCacheFileRef& file : files) {
                if (file->broken() || !file->validspec())
                    continue;
                if (nprinted++ >= topN)
                    break;
                Strutil::print(out, "    {}   {:9} ({:4.1f}%)   {}\n", nprinted,
                      Strutil::timeintervalformat(file->iotime()),
                      100.0 * file->iotime() / total_iotime,
                      onefile_stat_line(file, -1, false));
            }
            std::sort(files.begin(), files.end(), iorate_compare);
            Strutil::print(out, "  Files with slowest I/O rates:\n");
            nprinted = 0;
            for (const ImageCacheFileRef& file : files) {
                if (file->broken() || !file->validspec())
                    continue;
                if (file->iotime() < 0.25 && level <= 2)
                    continue;
                if (nprinted++ >= topN)
                    break;
                double mb = file->bytesread() / (1024.0 * 1024.0);
                double r  = mb / file->iotime();
                Strutil::print(out, "    {}   {:6.2f} MB/s ({:.2f}MB/{:.2f}s)   {}\n",
                      nprinted, r, mb, file->iotime(),
                      onefile_stat_line(file, -1, false));
            }
            if (nprinted == 0) {
                Strutil::print(out, "    (nothing took more than 0.25s)\n");
            } else {
                double fast = files.back()->bytesread() / (1024.0 * 1024.0)
                              / files.back()->iotime();
                Strutil::print(out, "    (fastest was {:.1f} MB/s)\n", fast);
            }
            if (total_redundant_tiles > 0) {
                std::sort(files.begin(), files.end(), redundantbytes_compare);
                Strutil::print(out, "  Top files by redundant I/O:\n");
                nprinted = 0;
                for (const ImageCacheFileRef& file : files) {
                    if (file->broken() || !file->validspec())
                        continue;
                    if (nprinted++ >= topN)
                        break;
                    Strutil::print(out, "    {}   {:6.1f} MB ({:4.1f}%)  {}\n", nprinted,
                          file->redundant_bytesread() / 1024.0 / 1024.0,
                          100.0 * file->redundant_bytesread()
                              / (double)total_redundant_bytes,
                          onefile_stat_line(file, -1, false));
                }
            }
        }
        int nbroken = 0;
        for (const ImageCacheFileRef& file : files) {
            if (file->broken())
                ++nbroken;
        }
        Strutil::print(out, "  Broken or invalid files: {}\n", nbroken);
        if (nbroken) {
            std::sort(files.begin(), files.end(), filename_compare);
            int nprinted = 0;
            for (const ImageCacheFileRef& file : files) {
                if (file->broken()) {
                    ++nprinted;
                    Strutil::print(out, "   {:4}  {}\n", nprinted, file->filename());
                }
            }
        }
    }

    return out.str();
}



void
ImageCacheImpl::printstats() const
{
    if (m_statslevel == 0)
        return;
    std::cout << getstats(m_statslevel) << "\n\n";
}



void
ImageCacheImpl::reset_stats()
{
    {
        spin_lock lock(m_perthread_info_mutex);
        for (size_t i = 0; i < m_all_perthread_info.size(); ++i)
            if (m_all_perthread_info[i])
                m_all_perthread_info[i]->m_stats.init();
    }

    {
        for (FilenameMap::iterator f = m_files.begin(); f != m_files.end();
             ++f) {
            const ImageCacheFileRef& file(f->second);
            file->m_timesopened = 0;
            file->m_tilesread   = 0;
            file->m_bytesread   = 0;
            file->m_iotime      = 0;
        }
    }
}



bool
ImageCacheImpl::attribute(string_view name, TypeDesc type, const void* val)
{
    bool do_invalidate    = false;
    bool force_invalidate = false;
    if (name == "options" && type == TypeDesc::STRING) {
        return optparser(*this, *(const char**)val);
    }
    if (name == "max_open_files" && type == TypeDesc::INT) {
        set_max_open_files(*(const int*)val);
    } else if (name == "max_memory_MB" && type == TypeDesc::FLOAT) {
        float size = *(const float*)val;
#ifdef NDEBUG
        size = std::max(size, 10.0f);  // Don't let users choose < 10 MB
#else
        size = std::max(size, 1.0f);  // But let developers debugging do it
#endif
        m_max_memory_bytes = (long long)(size * (long long)(1024 * 1024));
    } else if (name == "max_memory_MB" && type == TypeDesc::INT) {
        float size = *(const int*)val;
#ifdef NDEBUG
        size = std::max(size, 10.0f);  // Don't let users choose < 10 MB
#else
        size = std::max(size, 1.0f);  // But let developers debugging do it
#endif
        m_max_memory_bytes = (long long)(size * (long long)(1024 * 1024));
    } else if (name == "searchpath" && type == TypeDesc::STRING) {
        std::string s = std::string(*(const char**)val);
        if (s != m_searchpath) {
            m_searchpath = s;
            Filesystem::searchpath_split(m_searchpath, m_searchdirs, true);
            do_invalidate    = true;  // in case file can be found with new path
            force_invalidate = true;
        }
    } else if (name == "plugin_searchpath" && type == TypeDesc::STRING) {
        m_plugin_searchpath = std::string(*(const char**)val);
    } else if (name == "statistics:level" && type == TypeDesc::INT) {
        m_statslevel = *(const int*)val;
    } else if (name == "max_errors_per_file" && type == TypeDesc::INT) {
        m_max_errors_per_file = *(const int*)val;
    } else if (name == "autotile" && type == TypeDesc::INT) {
        int a = ceil2(*(const int*)val);  // guarantee pow2
        // Clamp to minimum 8x8 tiles to protect against stupid user who
        // think this is a boolean rather than the tile size.  Unless
        // we're in DEBUG mode, then allow developers to play with fire.
#ifdef NDEBUG
        if (a > 0 && a < 8)
            a = 8;
#endif
        if (a != m_autotile) {
            m_autotile    = a;
            do_invalidate = true;
        }
    } else if (name == "autoscanline" && type == TypeDesc::INT) {
        bool a = (*(const int*)val != 0);
        if (a != m_autoscanline) {
            m_autoscanline = a;
            do_invalidate  = true;
        }
    } else if (name == "automip" && type == TypeDesc::INT) {
        bool a = (*(const int*)val != 0);
        if (a != m_automip) {
            m_automip     = a;
            do_invalidate = true;
        }
    } else if (name == "forcefloat" && type == TypeDesc::INT) {
        bool a = (*(const int*)val != 0);
        if (a != m_forcefloat) {
            m_forcefloat  = a;
            do_invalidate = true;
        }
    } else if (name == "accept_untiled" && type == TypeDesc::INT) {
        bool a = (*(const int*)val != 0);
        if (a != m_accept_untiled) {
            m_accept_untiled = a;
            do_invalidate    = true;
        }
    } else if (name == "accept_unmipped" && type == TypeDesc::INT) {
        bool a = (*(const int*)val != 0);
        if (a != m_accept_unmipped) {
            m_accept_unmipped = a;
            do_invalidate     = true;
        }
    } else if (name == "deduplicate" && type == TypeDesc::INT) {
        bool r = (*(const int*)val != 0);
        if (r != m_deduplicate) {
            m_deduplicate = r;
            do_invalidate = true;
        }
    } else if (name == "unassociatedalpha" && type == TypeDesc::INT) {
        bool r = (*(const int*)val != 0);
        if (r != m_unassociatedalpha) {
            m_unassociatedalpha = r;
            do_invalidate       = true;
        }
    } else if (name == "failure_retries" && type == TypeDesc::INT) {
        m_failure_retries = *(const int*)val;
    } else if (name == "trust_file_extensions" && type == TypeDesc::INT) {
        m_trust_file_extensions = *(const int*)val;
    } else if (name == "max_open_files_strict" && type == TypeDesc::INT) {
        m_max_open_files_strict = *(const int*)val;
    } else if (name == "latlong_up" && type == TypeDesc::STRING) {
        bool y_up = !strcmp("y", *(const char**)val);
        if (y_up != m_latlong_y_up_default) {
            m_latlong_y_up_default = y_up;
            do_invalidate          = true;
        }
    } else if (name == "substitute_image" && type == TypeDesc::STRING) {
        ustring uval(*(const char**)val);
        if (uval != m_substitute_image) {
            m_substitute_image = uval;
            do_invalidate      = true;
        }
    } else if (name == "colorconfig" && type == TypeDesc::STRING) {
        ustring uval(*(const char**)val);
        if (uval != m_colorconfigname) {
            m_colorconfigname = uval;
            do_invalidate     = true;
        }
    } else if (name == "colorspace" && type == TypeDesc::STRING) {
        ustring uval(*(const char**)val);
        if (uval != m_colorspace) {
            m_colorspace  = uval;
            do_invalidate = true;
        }
    } else if (name == "max_mip_res" && type == TypeInt) {
        m_max_mip_res = *(const int*)val;
        do_invalidate = true;
    } else {
        // Otherwise, unknown name
        return false;
    }

    if (do_invalidate)
        invalidate_all(force_invalidate);
    return true;
}



TypeDesc
ImageCacheImpl::getattributetype(string_view name) const
{
    static std::unordered_map<std::string, TypeDesc> attr_types {
        { "max_open_files", TypeInt },
        { "max_memory_MB", TypeFloat },
        { "statistics:level", TypeInt },
        { "max_errors_per_file", TypeInt },
        { "autotile", TypeInt },
        { "autoscanline", TypeInt },
        { "automip", TypeInt },
        { "forcefloat", TypeInt },
        { "accept_untiled", TypeInt },
        { "accept_unmipped", TypeInt },
        { "deduplicate", TypeInt },
        { "unassociatedalpha", TypeInt },
        { "trust_file_extensions", TypeInt },
        { "failure_retries", TypeInt },
        { "total_files", TypeInt },
        { "max_mip_res", TypeInt },
        { "searchpath", TypeString },
        { "plugin_searchpath", TypeString },
        { "worldtocommon", TypeMatrix },
        { "commontoworld", TypeMatrix },
        { "latlong_up", TypeString },
        { "substitute_image", TypeString },
        { "stat:cache_memory_used", TypeInt64 },
        { "stat:tiles_created", TypeInt },
        { "stat:tiles_current", TypeInt },
        { "stat:tiles_peak", TypeInt },
        { "stat:open_files_created", TypeInt },
        { "stat:open_files_current", TypeInt },
        { "stat:open_files_peak", TypeInt },
        { "stat:find_tile_calls", TypeInt64 },
        { "stat:find_tile_microcache_misses", TypeInt64 },
        { "stat:find_tile_cache_misses", TypeInt },
        { "stat:files_totalsize", TypeInt64 },
        { "stat:image_size", TypeInt64 },
        { "stat:file_size", TypeInt64 },
        { "stat:bytes_read", TypeInt64 },
        { "stat:unique_files", TypeInt },
        { "stat:fileio_time", TypeFloat },
        { "stat:fileopen_time", TypeFloat },
        { "stat:file_locking_time", TypeFloat },
        { "stat:tile_locking_time", TypeFloat },
        { "stat:find_file_time", TypeFloat },
        { "stat:find_tile_time", TypeFloat },
        { "stat:texture_queries", TypeInt64 },
        { "stat:texture3d_queries", TypeInt64 },
        { "stat:environment_queries", TypeInt64 },
        { "stat:getimageinfo_queries", TypeInt64 },
        { "stat:gettextureinfo_queries", TypeInt64 },
    };

    // For all the easy cases, if the attribute is in the table and has a
    // simple type, use that.
    const auto found = attr_types.find(name);
    if (found != attr_types.end())
        return found->second;

    // The cases that don't fit in the table
    if (name == "all_filenames") {
        // all_filenames is an array, but the length depends on the actual
        // number of files we've encountered.
        return TypeDesc(TypeDesc::STRING, int(m_files.size()));
    }

    return TypeUnknown;
}



bool
ImageCacheImpl::getattribute(string_view name, TypeDesc type, void* val) const
{
#define ATTR_DECODE(_name, _ctype, _src)                         \
    if (name == _name && type == BaseTypeFromC<_ctype>::value) { \
        *(_ctype*)(val) = (_ctype)(_src);                        \
        return true;                                             \
    }

    ATTR_DECODE("max_open_files", int, m_max_open_files);
    ATTR_DECODE("max_memory_MB", float, m_max_memory_bytes / (1024.0 * 1024.0));
    ATTR_DECODE("max_memory_MB", int, m_max_memory_bytes / (1024 * 1024));
    ATTR_DECODE("statistics:level", int, m_statslevel);
    ATTR_DECODE("max_errors_per_file", int, m_max_errors_per_file);
    ATTR_DECODE("autotile", int, m_autotile);
    ATTR_DECODE("autoscanline", int, m_autoscanline);
    ATTR_DECODE("automip", int, m_automip);
    ATTR_DECODE("forcefloat", int, m_forcefloat);
    ATTR_DECODE("accept_untiled", int, m_accept_untiled);
    ATTR_DECODE("accept_unmipped", int, m_accept_unmipped);
    ATTR_DECODE("deduplicate", int, m_deduplicate);
    ATTR_DECODE("unassociatedalpha", int, m_unassociatedalpha);
    ATTR_DECODE("trust_file_extensions", int, m_trust_file_extensions);
    ATTR_DECODE("max_open_files_strict", int, m_max_open_files_strict);
    ATTR_DECODE("failure_retries", int, m_failure_retries);
    ATTR_DECODE("total_files", int, m_files.size());
    ATTR_DECODE("max_mip_res", int, m_max_mip_res);

    // The cases that don't fit in the simple ATTR_DECODE scheme
    if (name == "searchpath" && type == TypeDesc::STRING) {
        *(ustring*)val = m_searchpath;
        return true;
    }
    if (name == "plugin_searchpath" && type == TypeDesc::STRING) {
        *(ustring*)val = m_plugin_searchpath;
        return true;
    }
    if (name == "worldtocommon"
        && (type == TypeMatrix || type == TypeDesc(TypeDesc::FLOAT, 16))) {
        *(Imath::M44f*)val = m_Mw2c;
        return true;
    }
    if (name == "commontoworld"
        && (type == TypeMatrix || type == TypeDesc(TypeDesc::FLOAT, 16))) {
        *(Imath::M44f*)val = m_Mc2w;
        return true;
    }
    if (name == "latlong_up" && type == TypeDesc::STRING) {
        *(const char**)val = ustring(m_latlong_y_up_default ? "y" : "z").c_str();
        return true;
    }
    if (name == "substitute_image" && type == TypeDesc::STRING) {
        *(const char**)val = m_substitute_image.c_str();
        return true;
    }
    if (name == "colorconfig" && type == TypeDesc::STRING) {
        *(const char**)val = m_colorconfigname.c_str();
        return true;
    }
    if (name == "colorspace" && type == TypeDesc::STRING) {
        *(const char**)val = m_colorspace.c_str();
        return true;
    }
    if (name == "all_filenames" && type.basetype == TypeDesc::STRING
        && type.is_sized_array()) {
        ustring* names = (ustring*)val;
        int n          = type.arraylen;
        for (FilenameMap::iterator f = m_files.begin();
             f != m_files.end() && n-- > 0; ++f) {
            *names++ = f->second->filename();
        }
        return true;
    }

    if (Strutil::starts_with(name, "stat:")) {
        // Stats we can just grab
        ATTR_DECODE("stat:cache_footprint", long long, pvt::footprint(*this));
        ATTR_DECODE("stat:cache_memory_used", long long, m_mem_used);
        ATTR_DECODE("stat:tiles_created", int, m_stat_tiles_created);
        ATTR_DECODE("stat:tiles_current", int, m_stat_tiles_current);
        ATTR_DECODE("stat:tiles_peak", int, m_stat_tiles_peak);
        ATTR_DECODE("stat:open_files_created", int, m_stat_open_files_created);
        ATTR_DECODE("stat:open_files_current", int, m_stat_open_files_current);
        ATTR_DECODE("stat:open_files_peak", int, m_stat_open_files_peak);

        // All the other stats are those that need to be summed from all
        // the threads.
        ImageCacheStatistics stats;
        mergestats(stats);
        ATTR_DECODE("stat:find_tile_calls", long long, stats.find_tile_calls);
        ATTR_DECODE("stat:find_tile_microcache_misses", long long,
                    stats.find_tile_microcache_misses);
        ATTR_DECODE("stat:find_tile_cache_misses", int,
                    stats.find_tile_cache_misses);
        ATTR_DECODE("stat:files_totalsize", long long,
                    stats.files_totalsize);  // Old name
        ATTR_DECODE("stat:image_size", long long, stats.files_totalsize);
        ATTR_DECODE("stat:file_size", long long, stats.files_totalsize_ondisk);
        ATTR_DECODE("stat:bytes_read", long long, stats.bytes_read);
        ATTR_DECODE("stat:unique_files", int, stats.unique_files);
        ATTR_DECODE("stat:fileio_time", float, stats.fileio_time);
        ATTR_DECODE("stat:fileopen_time", float, stats.fileopen_time);
        ATTR_DECODE("stat:file_locking_time", float, stats.file_locking_time);
        ATTR_DECODE("stat:tile_locking_time", float, stats.tile_locking_time);
        ATTR_DECODE("stat:find_file_time", float, stats.find_file_time);
        ATTR_DECODE("stat:find_tile_time", float, stats.find_tile_time);
        ATTR_DECODE("stat:texture_queries", long long, stats.texture_queries);
        ATTR_DECODE("stat:texture3d_queries", long long,
                    stats.texture3d_queries);
        ATTR_DECODE("stat:environment_queries", long long,
                    stats.environment_queries);
        ATTR_DECODE("stat:getimageinfo_queries", long long,
                    stats.imageinfo_queries);
        ATTR_DECODE("stat:gettextureinfo_queries", long long,
                    stats.imageinfo_queries);
    }

    return false;
#undef ATTR_DECODE
}



bool
ImageCacheImpl::find_tile_main_cache(const TileID& id, ImageCacheTileRef& tile,
                                     ImageCachePerThreadInfo* thread_info)
{
    OIIO_DASSERT(!id.file().broken());
    ImageCacheStatistics& stats(thread_info->m_stats);

    ++stats.find_tile_microcache_misses;

    {
#if IMAGECACHE_TIME_STATS
        Timer timer1;
#endif
        bool found = m_tilecache.retrieve(id, tile);
#if IMAGECACHE_TIME_STATS
        stats.find_tile_time += timer1();
#endif
        if (found) {
            // We found the tile in the cache, but we need to make sure we
            // wait until the pixels are ready to read.  We purposely have
            // released the lock (above) before calling wait_pixels_ready,
            // otherwise we could deadlock if another thread reading the
            // pixels needs to lock the cache because it's doing automip.
            tile->wait_pixels_ready();
            tile->use();
            OIIO_DASSERT(id == tile->id());
            OIIO_DASSERT(tile);
            return true;
        }
    }

    // The tile was not found in cache.

    ++stats.find_tile_cache_misses;

    // Yes, we're creating and reading a tile with no lock -- this is to
    // prevent all the other threads from blocking because of our
    // expensive disk read.  We believe this is safe, since underneath
    // the ImageCacheFile will lock itself for the read_tile and there are
    // no other non-threadsafe side effects.
    tile = new ImageCacheTile(id);
    // N.B. the ImageCacheTile ctr starts the tile out as 'used'
    OIIO_DASSERT(tile);
    OIIO_DASSERT(id == tile->id());

    bool ok = add_tile_to_cache(tile, thread_info);
    OIIO_DASSERT(id == tile->id());
    return ok && tile->valid();
}



bool
ImageCacheImpl::add_tile_to_cache(ImageCacheTileRef& tile,
                                  ImageCachePerThreadInfo* thread_info)
{
    bool ourtile = m_tilecache.insert_retrieve(tile->id(), tile, tile);

    // If we added a new tile to the cache, we may still need to read the
    // pixels; and if we found the tile in cache, we may need to wait for
    // somebody else to read the pixels.
    bool ok = true;
    if (ourtile) {
        if (!tile->pixels_ready()) {
            Timer timer;
            ok              = tile->read(thread_info);
            double readtime = timer();
            thread_info->m_stats.fileio_time += readtime;
            tile->id().file().iotime() += readtime;
        }
        check_max_mem(thread_info);
    } else {
        // Somebody else already added the tile to the cache before we
        // could, so we'll use their reference, but we need to wait until it
        // has read in the pixels.
        tile->wait_pixels_ready();
    }
    return ok;
}



void
ImageCacheImpl::check_max_mem(ImageCachePerThreadInfo* /*thread_info*/)
{
    OIIO_DASSERT(m_mem_used < (long long)m_max_memory_bytes * 10);  // sanity
#if 0
    static atomic_int n;
    if (! (n++ % 64) || m_mem_used >= (long long)m_max_memory_bytes)
        std::cerr << "mem used: " << m_mem_used << ", max = " << m_max_memory_bytes << "\n";
#endif
    // Early out if the cache is empty
    if (m_tilecache.empty())
        return;
    // Early out if we aren't exceeding the tile memory limit
    if (m_mem_used < (long long)m_max_memory_bytes)
        return;

    // Try to grab the tile_sweep_mutex lock. If somebody else holds it,
    // just return -- leave the memory limit enforcement to whomever is
    // already in this function, no need for two threads to do it at
    // once.  If this means we may ephemerally be over the memory limit
    // (because another thread adds a tile before we have freed enough
    // here), so be it.
    if (!m_tile_sweep_mutex.try_lock())
        return;

    // Now, what we want to do is have a "clock hand" that sweeps across
    // the cache, releasing tiles that haven't been used for a long
    // time.  Because of multi-thread, rather than keep an iterator
    // around for this (which could be invalidated since the last time
    // we used it), we just remember the tileID of the next tile to
    // check, then look it up fresh.  That is m_tile_sweep_id.

    // Get a (locked) iterator for the next tile to be examined.
    TileCache::iterator sweep;
    if (!m_tile_sweep_id.empty()) {
        // We saved the sweep_id. Find the iterator corresponding to it.
        sweep = m_tilecache.find(m_tile_sweep_id);
        // Note: if the sweep_id is no longer in the table, sweep will be an
        // empty iterator. That's ok, it will be fixed early in the main
        // loop below.
    }

    // Loop while we still use too much tile memory.  Also, be careful
    // of looping for too long, exit the loop if we just keep spinning
    // uncontrollably.
    int full_loops = 0;
    while (m_mem_used >= (long long)m_max_memory_bytes && full_loops < 100) {
        // If we have fallen off the end of the cache, loop back to the
        // beginning and increment our full_loops count.
        if (!sweep) {
            sweep = m_tilecache.begin();
            ++full_loops;
        }
        // If we're STILL at the end, it must be that somehow the entire
        // cache is empty.  So just declare ourselves done.
        if (!sweep)
            break;
        OIIO_DASSERT(sweep->second);

        if (!sweep->second->release()) {
            // This is a tile we should delete.  To keep iterating
            // safely, we have a good trick:
            // 1. remember the TileID of the tile to delete
            TileID todelete = sweep->first;
            size_t size     = sweep->second->memsize();
            OIIO_DASSERT(m_mem_used >= (long long)size);
            // 2. Find the TileID of the NEXT item. We do this by
            // incrementing the sweep iterator and grabbing its id.
            ++sweep;
            m_tile_sweep_id = (sweep ? sweep->first : TileID());
            // 3. Release the bin lock and erase the tile we wish to delete.
            sweep.unlock();
            m_tilecache.erase(todelete);
            // 4. Re-establish a locked iterator for the next item, since
            // the old iterator may have been invalidated by the erasure.
            if (!m_tile_sweep_id.empty())
                sweep = m_tilecache.find(m_tile_sweep_id);
        } else {
            ++sweep;
        }
    }

    // OK, by this point we have either freed enough tiles to be below
    // the limit again, or the cache is empty, or we've looped over the
    // cache too many times and are giving up.

    // Now we must save the tileid for next time.  Just set it to an
    // empty ID if we don't have a valid iterator at this point.
    m_tile_sweep_id = (sweep ? sweep->first : TileID());
    m_tile_sweep_mutex.unlock();

    // N.B. As we exit, the iterators will go out of scope and we will
    // retain no locks on the cache.
}



std::string
ImageCacheImpl::resolve_filename(const std::string& filename) const
{
    // Ask if the format can generate imagery procedurally. If so, don't
    // go looking for a file.
    if (pvt::is_procedural_plugin(filename)
        || pvt::is_procedural_plugin(Filesystem::extension(filename, false)))
        return filename;
    if (m_searchdirs.empty() || Filesystem::path_is_absolute(filename, true)) {
        // Don't bother with the searchpath_find call since it will do an
        // existence check that we don't need.
        return filename;
    }
    std::string s = Filesystem::searchpath_find(filename, m_searchdirs, true);
    return s.empty() ? filename : s;
}



bool
ImageCacheImpl::get_image_info(ustring filename, int subimage, int miplevel,
                               ustring dataname, TypeDesc datatype, void* data)
{
    ImageCachePerThreadInfo* thread_info = get_perthread_info();
    ImageCacheFile* file = find_file(filename, thread_info, nullptr);
    if (!file && dataname != s_exists) {
        error("Invalid image file \"{}\"", filename);
        return false;
    }
    return get_image_info(file, thread_info, subimage, miplevel, dataname,
                          datatype, data);
}



bool
ImageCacheImpl::get_image_info(ImageCacheFile* file,
                               ImageCachePerThreadInfo* thread_info,
                               int subimage, int miplevel, ustring dataname,
                               TypeDesc datatype, void* data)
{
#define ATTR_DECODE(_name, _ctype, _src)                                 \
    if (dataname == _name && datatype == BaseTypeFromC<_ctype>::value) { \
        *(_ctype*)(data) = (_ctype)(_src);                               \
        return true;                                                     \
    }

    if (!thread_info)
        thread_info = get_perthread_info();
    ImageCacheStatistics& stats(thread_info->m_stats);
    ++stats.imageinfo_queries;
    file = verify_file(file, thread_info, true);
    if (dataname == s_exists && datatype == TypeInt) {
        // Just check for existence.  Need to do this before the invalid
        // file error below, since in this one case, it's not an error
        // for the file to be nonexistent or broken!
        if ((*(int*)data = (file && !file->broken())) == 0) {
            // eat any error generated by find_file
            imcache_error_messages.erase(imagecache_id);
            (void)OIIO::geterror(true);  // Suppress global errors
        }
        return true;
    }
    if (!file) {
        error("Invalid image file");
        return false;
    }
    ATTR_DECODE(s_broken, int, file->broken());
    if (Strutil::starts_with(dataname, "stat:")) {
        ATTR_DECODE("stat:tilesread", long long, file->m_tilesread);
        ATTR_DECODE("stat:bytesread", long long, file->m_bytesread);
        ATTR_DECODE("stat:redundant_tiles", long long, file->m_redundant_tiles);
        ATTR_DECODE("stat:redundant_bytesread", long long,
                    file->m_redundant_bytesread);
        ATTR_DECODE("stat:timesopened", int, file->m_timesopened);
        ATTR_DECODE("stat:iotime", float, file->m_iotime);
        ATTR_DECODE("stat:mipused", int, file->m_mipused);
        ATTR_DECODE("stat:is_duplicate", int, bool(file->duplicate()));
        ATTR_DECODE("stat:image_size", long long, file->m_total_imagesize);
        ATTR_DECODE("stat:file_size", long long,
                    file->m_total_imagesize_ondisk);
    }

    if (file->broken()) {
        if (file->errors_should_issue()) {
            // eat any earlier messages
            imcache_error_messages.erase(imagecache_id);
            error("Invalid image file \"{}\": {}", file->filename(),
                  file->broken_error_message());
        }
        return false;
    }
    // No other queries below are expected to work with broken

    if (dataname == s_UDIM && datatype == TypeInt) {
        // Just check for existence.  Need to do this before the invalid
        // file error below, since in this one case, it's not an error
        // for the file to be nonexistent or broken!
        *(int*)data = file->is_udim();
        return true;
    }

    if (file->is_udim()) {
        // UDIMs are tricky. What we want to do here is successfully
        // retrieve the attribute if it's the same for all UDIM tiles, but
        // fail if it's not the same for all tiles.
        size_t size  = datatype.size();
        char* common = OIIO_ALLOCA(char, size);
        char* single = OIIO_ALLOCA(char, size);
        bool first   = true;
        // Search the known set of concrete filenames comprising this UDIM.
        // If file->is_udim() is true, the map's length won't be changed
        // again, so it's safe to freely iterate over it without locking.
        for (auto& f : file->m_udim_lookup) {
            if (f.filename.empty())
                continue;  // empty filename means tile is unpopulated
            ImageCacheFile* concretefile = resolve_udim(file, thread_info, f.u,
                                                        f.v);
            concretefile = verify_file(concretefile, thread_info, true);
            if (concretefile && !concretefile->broken()) {
                // Recurse to try again with the concrete file. Store the
                // attribute either in `common` if this is the first one
                // we've retrieved, otherwise in `single`.
                bool r = get_image_info(concretefile, thread_info, subimage,
                                        miplevel, dataname, datatype,
                                        first ? common : single);
                if (!r) {
                    // If we couldn't retrieve the attribute from at
                    // least one existing file, it's a failure.
                    (void)geterror();  // suppress errors from missing tiles
                    return false;
                }
                if (first) {
                    first = false;
                } else {
                    // If this is not the first tile we found, make sure
                    // the attribute has the same value as prior finds.
                    if (memcmp(common, single, size) != 0)
                        return false;
                }
            }
        }
        // If we got this far, all the tiles we found had the attribute, and
        // they all had the same value. Copy to the user and return success.
        memcpy(data, common, size);
        return true;  // UDIM-like files fail all other queries
    }
    if (dataname == s_subimages && datatype == TypeInt) {
        *(int*)data = file->subimages();
        return true;
    }

    // Make sure we have a valid subimage and miplevel BEFORE we get a
    // reference to the spec.
    if (subimage < 0 || subimage >= file->subimages()) {
        if (file->errors_should_issue())
            error("Unknown subimage {} (out of {})", subimage,
                  file->subimages());
        return false;
    }
    if (miplevel < 0 || miplevel >= file->miplevels(subimage)) {
        if (file->errors_should_issue())
            error("Unknown mip level {} (out of {})", miplevel,
                  file->miplevels(subimage));
        return false;
    }

    const SubimageInfo& si(file->subimageinfo(subimage));
    const ImageSpec& spec(si.spec());
    const ImageDims& dims(si.leveldims(miplevel));
    if (dataname == s_resolution && datatype == TypeDesc(TypeDesc::INT, 2)) {
        int* d = (int*)data;
        d[0]   = dims.width;
        d[1]   = dims.height;
        return true;
    }
    if (dataname == s_resolution && datatype == TypeDesc(TypeDesc::INT, 3)) {
        int* d = (int*)data;
        d[0]   = dims.width;
        d[1]   = dims.height;
        d[2]   = dims.depth;
        return true;
    }
    if (dataname == s_texturetype && datatype == TypeString) {
        ustring s(texture_type_name(file->textureformat()));
        *(const char**)data = s.c_str();
        return true;
    }
    if (dataname == s_textureformat && datatype == TypeString) {
        ustring s(texture_format_name(file->textureformat()));
        *(const char**)data = s.c_str();
        return true;
    }
    if (dataname == s_fileformat && datatype == TypeString) {
        *(const char**)data = file->fileformat().c_str();
        return true;
    }
    if (dataname == s_channels && datatype == TypeInt) {
        *(int*)data = spec.nchannels;
        return true;
    }
    if (dataname == s_channels && datatype == TypeFloat) {
        *(float*)data = spec.nchannels;
        return true;
    }
    if (dataname == s_format && datatype == TypeInt) {
        *(int*)data = (int)spec.format.basetype;
        return true;
    }
    if ((dataname == s_cachedformat || dataname == s_cachedpixeltype)
        && datatype == TypeInt) {
        *(int*)data = (int)file->datatype(subimage).basetype;
        return true;
    }
    if (dataname == s_miplevels && datatype == TypeInt) {
        *(int*)data = file->miplevels(subimage);
        return true;
    }
    if (dataname == s_datawindow && datatype.basetype == TypeDesc::INT
        && (datatype == TypeDesc(TypeDesc::INT, 4)
            || datatype == TypeDesc(TypeDesc::INT, 6))) {
        int* d = (int*)data;
        if (datatype.arraylen == 4) {
            d[0] = dims.x;
            d[1] = dims.y;
            d[2] = dims.x + dims.width - 1;
            d[3] = dims.y + dims.height - 1;
        } else {
            d[0] = dims.x;
            d[1] = dims.y;
            d[2] = dims.z;
            d[3] = dims.x + dims.width - 1;
            d[4] = dims.y + dims.height - 1;
            d[5] = dims.z + dims.depth - 1;
        }
        return true;
    }
    if (dataname == s_displaywindow && datatype.basetype == TypeDesc::INT
        && (datatype == TypeDesc(TypeDesc::INT, 4)
            || datatype == TypeDesc(TypeDesc::INT, 6))) {
        int* d = (int*)data;
        if (datatype.arraylen == 4) {
            d[0] = dims.full_x;
            d[1] = dims.full_y;
            d[2] = dims.full_x + dims.full_width - 1;
            d[3] = dims.full_y + dims.full_height - 1;
        } else {
            d[0] = dims.full_x;
            d[1] = dims.full_y;
            d[2] = dims.full_z;
            d[3] = dims.full_x + dims.full_width - 1;
            d[4] = dims.full_y + dims.full_height - 1;
            d[5] = dims.full_z + dims.full_depth - 1;
        }
        return true;
    }
    if (dataname == s_averagecolor && datatype.basetype == TypeDesc::FLOAT) {
        int datalen = datatype.numelements() * datatype.aggregate;
        return file->get_average_color((float*)data, subimage, 0, datalen);
    }
    if (dataname == s_averagealpha && datatype == TypeDesc::FLOAT
        && spec.alpha_channel >= 0) {
        return file->get_average_color((float*)data, subimage,
                                       spec.alpha_channel,
                                       spec.alpha_channel + 1);
    }
    if (dataname == s_constantcolor && datatype.basetype == TypeDesc::FLOAT) {
        if (file->subimageinfo(subimage).is_constant_image) {
            int datalen = datatype.numelements() * datatype.aggregate;
            return file->get_average_color((float*)data, subimage, 0, datalen);
        }
        return false;  // Fail if it's not a constant image
    }
    if (dataname == s_constantalpha && datatype == TypeDesc::FLOAT
        && spec.alpha_channel >= 0) {
        if (file->subimageinfo(subimage).is_constant_image)
            return file->get_average_color((float*)data, subimage,
                                           spec.alpha_channel,
                                           spec.alpha_channel + 1);
        else
            return false;  // Fail if it's not a constant image
    }

    // general case -- handle anything else that's able to be found by
    // spec.find_attribute().
    ParamValue tmpparam;
    const ParamValue* p = spec.find_attribute(dataname, tmpparam);
    if (p && p->type().basevalues() == datatype.basevalues()) {
        // First test for exact base type match
        if (p->type().basetype == datatype.basetype) {
            memcpy(data, p->data(), datatype.size());
            return true;
        }
        // If the real data is int but user asks for float, translate it
        if (p->type().basetype == TypeDesc::INT
            && datatype.basetype == TypeDesc::FLOAT) {
            for (int i = 0; i < int(p->type().basevalues()); ++i)
                ((float*)data)[i] = ((int*)p->data())[i];
            return true;
        }
    }

    return false;

#undef ATTR_DECODE
}



bool
ImageCacheImpl::get_imagespec(ustring filename, ImageSpec& spec, int subimage)
{
    const ImageSpec* specptr = imagespec(filename, subimage);
    if (specptr) {
        spec = *specptr;
        return true;
    } else {
        return false;  // imagespec() already handled the errors
    }
}



bool
ImageCacheImpl::get_imagespec(ImageCacheFile* file,
                              ImageCachePerThreadInfo* thread_info,
                              ImageSpec& spec, int subimage)
{
    const ImageSpec* specptr = imagespec(file, thread_info, subimage);
    if (specptr) {
        spec = *specptr;
        return true;
    } else {
        return false;  // imagespec() already handled the errors
    }
}



const ImageSpec*
ImageCacheImpl::imagespec(ustring filename, int subimage)
{
    ImageCachePerThreadInfo* thread_info = get_perthread_info();
    ImageCacheFile* file = find_file(filename, thread_info, nullptr);
    if (!file) {
        error("Image file \"{}\" not found", filename);
        return NULL;
    }
    return imagespec(file, thread_info, subimage);
}



const ImageSpec*
ImageCacheImpl::imagespec(ImageCacheFile* file,
                          ImageCachePerThreadInfo* thread_info, int subimage)
{
    if (!file) {
        error("Image file handle was NULL");
        return NULL;
    }
    if (!thread_info)
        thread_info = get_perthread_info();
    file = verify_file(file, thread_info, true);
    if (file->broken()) {
        if (file->errors_should_issue())
            error("Invalid image file \"{}\": {}", file->filename(),
                  file->broken_error_message());
        return NULL;
    }
    if (file->is_udim()) {
        error("Cannot retrieve ImageSpec of a UDIM-like virtual file");
        return NULL;  // UDIM-like files don't have an ImageSpec
    }
    if (subimage < 0 || subimage >= file->subimages()) {
        if (file->errors_should_issue())
            error("Unknown subimage {} (out of {})", subimage,
                  file->subimages());
        return NULL;
    }

    //! force mip level to zero for now
    //! TODO: store nativespec in SubImageInfo, and extra overrides in LevelInfo
    constexpr int miplevel = 0;
    if (miplevel < 0 || miplevel >= file->miplevels(subimage)) {
        if (file->errors_should_issue())
            error("Unknown mip level {} (out of {})", miplevel,
                  file->miplevels(subimage));
        return NULL;
    }
    return &file->spec(subimage);
}



bool
ImageCacheImpl::get_cache_dimensions(ustring filename, ImageSpec& spec,
                                     int subimage, int miplevel)
{
    ImageCachePerThreadInfo* thread_info = get_perthread_info();
    ImageCacheFile* file = find_file(filename, thread_info, nullptr);
    if (!file) {
        error("Image file \"{}\" not found", filename);
        return false;
    }
    return get_cache_dimensions(file, thread_info, spec, subimage, miplevel);
}



bool
ImageCacheImpl::get_cache_dimensions(ImageCacheFile* file,
                                     ImageCachePerThreadInfo* thread_info,
                                     ImageSpec& spec, int subimage,
                                     int miplevel)
{
    if (!file) {
        error("Image file handle was NULL");
        return false;
    }
    if (!thread_info)
        thread_info = get_perthread_info();
    file = verify_file(file, thread_info, true);
    if (file->broken()) {
        if (file->errors_should_issue())
            error("Invalid image file \"{}\": {}", file->filename(),
                  file->broken_error_message());
        return false;
    }
    if (file->is_udim()) {
        error("Cannot retrieve ImageSpec of a UDIM-like virtual file");
        return false;  // UDIM-like files don't have an ImageSpec
    }
    if (subimage < 0 || subimage >= file->subimages()) {
        if (file->errors_should_issue())
            error("Unknown subimage {} (out of {})", subimage,
                  file->subimages());
        return false;
    }
    if (miplevel < 0 || miplevel >= file->miplevels(subimage)) {
        if (file->errors_should_issue())
            error("Unknown mip level {} (out of {})", miplevel,
                  file->miplevels(subimage));
        return false;
    }
    //! copy dimensions from mip level ImageSpec
    const SubimageInfo& si(file->subimageinfo(subimage));
    si.get_cache_dimensions(miplevel, spec);
    return true;
}



bool
ImageCacheImpl::get_thumbnail(ustring filename, ImageBuf& thumb, int subimage)
{
    ImageCachePerThreadInfo* thread_info = get_perthread_info();
    ImageCacheFile* file = find_file(filename, thread_info, nullptr);
    if (!file) {
        error("Image file \"{}\" not found", filename);
        return false;
    }
    return get_thumbnail(file, thread_info, thumb, subimage);
}



bool
ImageCacheImpl::get_thumbnail(ImageCacheFile* file,
                              ImageCachePerThreadInfo* thread_info,
                              ImageBuf& thumb, int subimage)
{
    std::shared_ptr<ImageInput> inp = file->open(thread_info);
    if (!inp)
        return false;  // indicates a broken file
    return inp->get_thumbnail(thumb, subimage);
}



int
ImageCacheImpl::subimage_from_name(ImageCacheFile* file, ustring subimagename)
{
    for (int s = 0, send = file->subimages(); s < send; ++s) {
        if (file->subimageinfo(s).subimagename == subimagename)
            return s;
    }
    return -1;  // No matching subimage name
}



bool
ImageCacheImpl::get_pixels(ustring filename, int subimage, int miplevel,
                           int xbegin, int xend, int ybegin, int yend,
                           int zbegin, int zend, TypeDesc format, void* result)
{
    return get_pixels(filename, subimage, miplevel, xbegin, xend, ybegin, yend,
                      zbegin, zend, 0, -1, format, result);
}



bool
ImageCacheImpl::get_pixels(ImageCacheFile* file,
                           ImageCachePerThreadInfo* thread_info, int subimage,
                           int miplevel, int xbegin, int xend, int ybegin,
                           int yend, int zbegin, int zend, TypeDesc format,
                           void* result)
{
    return get_pixels(file, thread_info, subimage, miplevel, xbegin, xend,
                      ybegin, yend, zbegin, zend, 0, -1, format, result);
}



bool
ImageCacheImpl::get_pixels(ustring filename, int subimage, int miplevel,
                           int xbegin, int xend, int ybegin, int yend,
                           int zbegin, int zend, int chbegin, int chend,
                           TypeDesc format, void* result, stride_t xstride,
                           stride_t ystride, stride_t zstride,
                           int cache_chbegin, int cache_chend)
{
    ImageCachePerThreadInfo* thread_info = get_perthread_info();
    ImageCacheFile* file                 = find_file(filename, thread_info);
    if (!file) {
        error("Image file \"{}\" not found", filename);
        return false;
    }
    return get_pixels(file, thread_info, subimage, miplevel, xbegin, xend,
                      ybegin, yend, zbegin, zend, chbegin, chend, format,
                      result, xstride, ystride, zstride, cache_chbegin,
                      cache_chend);
}



bool
ImageCacheImpl::get_pixels(ImageCacheFile* file,
                           ImageCachePerThreadInfo* thread_info, int subimage,
                           int miplevel, int xbegin, int xend, int ybegin,
                           int yend, int zbegin, int zend, int chbegin,
                           int chend, TypeDesc format, void* result,
                           stride_t xstride, stride_t ystride, stride_t zstride,
                           int cache_chbegin, int cache_chend)
{
    if (!thread_info)
        thread_info = get_perthread_info();
    file = verify_file(file, thread_info);
    if (file->broken()) {
        if (file->errors_should_issue())
            error("Invalid image file \"{}\": {}", file->filename(),
                  file->broken_error_message());
        return false;
    }
    if (file->is_udim()) {
        error("Cannot get_pixels() of a UDIM-like virtual file");
        return false;  // UDIM-like files don't have pixels
    }
    if (subimage < 0 || subimage >= file->subimages()) {
        if (file->errors_should_issue())
            error("get_pixels asked for nonexistent subimage {} of \"{}\"",
                  subimage, file->filename());
        return false;
    }
    if (miplevel < 0 || miplevel >= file->miplevels(subimage)) {
        if (file->errors_should_issue())
            error("get_pixels asked for nonexistent MIP level {} of \"{}\"",
                  miplevel, file->filename());
        return false;
    }

    if (!thread_info)
        thread_info = get_perthread_info();
    const SubimageInfo& si(file->subimageinfo(subimage));
    const ImageDims& dims(si.leveldims(miplevel));
    bool ok = true;

    // Compute channels and stride if not given (assume all channels,
    // contiguous data layout for strides).
    if (chbegin < 0 || chend < 0) {
        chbegin = 0;
        chend   = dims.nchannels;
    }
    int result_nchans = chend - chbegin;
    if (cache_chbegin < 0 || cache_chend < 0 || cache_chbegin > chbegin
        || cache_chend < chend) {
        cache_chbegin = 0;
        cache_chend   = dims.nchannels;
    }
    int cache_nchans = cache_chend - cache_chbegin;
    ImageSpec::auto_stride(xstride, ystride, zstride, format, result_nchans,
                           xend - xbegin, yend - ybegin);

    // result_pixelsize, scanlinesize, and zplanesize assume contiguous
    // layout.  This may or may not be the same as the strides passed by
    // the caller.
    TypeDesc cachetype          = file->datatype(subimage);
    const size_t cachesize      = cachetype.size();
    const stride_t cache_stride = cachesize * cache_nchans;
    size_t formatsize           = format.size();
    stride_t result_pixelsize   = result_nchans * formatsize;
    bool xcontig                = (result_pixelsize == xstride
                    && result_nchans == cache_nchans);
    stride_t scanlinesize       = (xend - xbegin) * result_pixelsize;
    stride_t zplanesize         = (yend - ybegin) * scanlinesize;
    OIIO_DASSERT(dims.depth >= 1 && dims.tile_depth >= 1);

    imagesize_t npixelsread = 0;
    char* zptr              = (char*)result;
    for (int z = zbegin; z < zend; ++z, zptr += zstride) {
        if (z < dims.z || z >= (dims.z + dims.depth)) {
            // nonexistent planes
            if (xstride == result_pixelsize && ystride == scanlinesize) {
                // Can zero out the plane in one shot
                memset(zptr, 0, zplanesize);
            } else {
                // Non-contiguous strides -- zero out individual pixels
                char* yptr = zptr;
                for (int y = ybegin; y < yend; ++y, yptr += ystride) {
                    char* xptr = yptr;
                    for (int x = xbegin; x < xend; ++x, xptr += xstride)
                        memset(xptr, 0, result_pixelsize);
                }
            }
            continue;
        }
        int old_tx = -100000, old_ty = -100000, old_tz = -100000;
        int tz     = z - ((z - dims.z) % dims.tile_depth);
        char* yptr = zptr;
        int ty     = ybegin - ((ybegin - dims.y) % dims.tile_height);
        int tyend  = ty + dims.tile_height;
        for (int y = ybegin; y < yend; ++y, yptr += ystride) {
            if (y == tyend) {
                ty = tyend;
                tyend += dims.tile_height;
            }
            if (y < dims.y || y >= (dims.y + dims.height)) {
                // nonexistent scanlines
                if (xstride == result_pixelsize) {
                    // Can zero out the scanline in one shot
                    memset(yptr, 0, scanlinesize);
                } else {
                    // Non-contiguous strides -- zero out individual pixels
                    char* xptr = yptr;
                    for (int x = xbegin; x < xend; ++x, xptr += xstride)
                        memset(xptr, 0, result_pixelsize);
                }
                continue;
            }
            // int ty = y - ((y - dims.y) % dims.tile_height);
            char* xptr       = yptr;
            const char* data = NULL;
            for (int x = xbegin; x < xend;
                 ++x, xptr += xstride, ++npixelsread) {
                if (x < dims.x || x >= (dims.x + dims.width)) {
                    // nonexistent columns
                    memset(xptr, 0, result_pixelsize);
                    continue;
                }
                int tx = x - ((x - dims.x) % dims.tile_width);
                if (old_tx != tx || old_ty != ty || old_tz != tz) {
                    // Only do a find_tile and re-setup of the data
                    // pointer when we move across a tile boundary.
                    TileID tileid(*file, subimage, miplevel, tx, ty, tz,
                                  cache_chbegin, cache_chend);
                    ok &= find_tile(tileid, thread_info, npixelsread == 0);
                    if (!ok)
                        return false;  // Just stop if file read failed
                    old_tx = tx;
                    old_ty = ty;
                    old_tz = tz;
                    data   = NULL;
                }
                if (!data) {
                    ImageCacheTileRef& tile(thread_info->tile);
                    OIIO_DASSERT(tile);
                    data = (const char*)tile->data(x, y, z, chbegin);
                    OIIO_DASSERT(data);
                }
                if (xcontig) {
                    // Special case for a contiguous span within one tile
                    int spanend   = std::min(tx + dims.tile_width, xend);
                    stride_t span = spanend - x;
                    convert_pixel_values(cachetype, data, format, xptr,
                                         result_nchans * span);
                    x += (span - 1);
                    xptr += xstride * (span - 1);
                    // no need to increment data, since next read will
                    // be from a different tile
                } else {
                    convert_pixel_values(cachetype, data, format, xptr,
                                         result_nchans);
                    data += cache_stride;
                }
            }
        }
    }

    return ok;
}



bool
ImageCacheImpl::get_pixels(ImageCacheFile* file,
                           ImageCachePerThreadInfo* thread_info, int subimage,
                           int miplevel, const ROI& roi, TypeDesc format,
                           const image_span<std::byte>& result,
                           int cache_chbegin, int cache_chend)
{
    // Default implementation (for now): call the old pointer+stride
    return get_pixels(file, thread_info, subimage, miplevel, roi.xbegin,
                      roi.xend, roi.ybegin, roi.yend, roi.zbegin, roi.zend,
                      roi.chbegin, roi.chend, format, result.data(),
                      result.xstride(), result.ystride(), result.zstride(),
                      cache_chbegin, cache_chend);
}



bool
ImageCache::get_pixels(ustring filename, int subimage, int miplevel,
                       const ROI& roi, TypeDesc format,
                       const image_span<std::byte>& result, int cache_chbegin,
                       int cache_chend)
{
    return m_impl->get_pixels(filename, subimage, miplevel, roi.xbegin,
                              roi.xend, roi.ybegin, roi.yend, roi.zbegin,
                              roi.zend, roi.chbegin, roi.chend, format,
                              result.data(), result.xstride(), result.ystride(),
                              result.zstride(), cache_chbegin, cache_chend);
}



bool
ImageCache::get_pixels(ImageCacheFile* file,
                       ImageCachePerThreadInfo* thread_info, int subimage,
                       int miplevel, const ROI& roi, TypeDesc format,
                       const image_span<std::byte>& result, int cache_chbegin,
                       int cache_chend)
{
    return m_impl->get_pixels(file, thread_info, subimage, miplevel, roi.xbegin,
                              roi.xend, roi.ybegin, roi.yend, roi.zbegin,
                              roi.zend, roi.chbegin, roi.chend, format,
                              result.data(), result.xstride(), result.ystride(),
                              result.zstride(), cache_chbegin, cache_chend);
}



bool
ImageCacheImpl::get_pixels(ustring filename, int subimage, int miplevel,
                           const ROI& roi, TypeDesc format,
                           const image_span<std::byte>& result,
                           int cache_chbegin, int cache_chend)
{
    ImageCachePerThreadInfo* thread_info = get_perthread_info();
    ImageCacheFile* file                 = find_file(filename, thread_info);
    if (!file) {
        error("Image file \"{}\" not found", filename);
        return false;
    }
    return get_pixels(file, thread_info, subimage, miplevel, roi.xbegin,
                      roi.xend, roi.ybegin, roi.yend, roi.zbegin, roi.zend,
                      roi.chbegin, roi.chend, format, result.data(),
                      result.xstride(), result.ystride(), result.zstride(),
                      cache_chbegin, cache_chend);
}



ImageCache::Tile*
ImageCacheImpl::get_tile(ustring filename, int subimage, int miplevel, int x,
                         int y, int z, int chbegin, int chend)
{
    ImageCachePerThreadInfo* thread_info = get_perthread_info();
    ImageCacheFile* file                 = find_file(filename, thread_info);
    return get_tile(file, thread_info, subimage, miplevel, x, y, z, chbegin,
                    chend);
}



ImageCache::Tile*
ImageCacheImpl::get_tile(ImageHandle* file, Perthread* thread_info,
                         int subimage, int miplevel, int x, int y, int z,
                         int chbegin, int chend)
{
    if (!thread_info)
        thread_info = get_perthread_info();
    file = verify_file(file, thread_info);
    if (!file || file->broken() || file->is_udim())
        return nullptr;
    if (subimage < 0 || subimage >= file->subimages() || miplevel < 0
        || miplevel >= file->miplevels(subimage))
        return nullptr;
    const SubimageInfo& si(file->subimageinfo(subimage));
    const ImageDims& dims(si.leveldims(miplevel));
    // Snap x,y,z to the corner of the tile
    int xtile = (x - dims.x) / dims.tile_width;
    int ytile = (y - dims.y) / dims.tile_height;
    int ztile = (z - dims.z) / dims.tile_depth;
    x         = dims.x + xtile * dims.tile_width;
    y         = dims.y + ytile * dims.tile_height;
    z         = dims.z + ztile * dims.tile_depth;
    if (chend < chbegin) {  // chend < chbegin means "all channels."
        chbegin = 0;
        chend   = dims.nchannels;
    }
    TileID id(*file, subimage, miplevel, x, y, z, chbegin, chend);
    if (find_tile(id, thread_info, true)) {
        ImageCacheTileRef tile(thread_info->tile);
        tile->_incref();  // Fake an extra reference count
        return (ImageCache::Tile*)tile.get();
    } else {
        return NULL;
    }
}



void
ImageCacheImpl::release_tile(ImageCache::Tile* tile) const
{
    if (!tile)
        return;
    ImageCacheTileRef tileref((ImageCacheTile*)tile);
    tileref->use();
    tileref->_decref();  // Reduce ref count that we bumped in get_tile
    // when we exit scope, tileref will do the final dereference
}



TypeDesc
ImageCacheImpl::tile_format(const Tile* tile) const
{
    const TileID& id(((const ImageCacheTile*)tile)->id());
    const ImageSpec& spec(id.file().spec(id.subimage()));
    return spec.format;
}



ROI
ImageCacheImpl::tile_roi(const Tile* tile) const
{
    const TileID& id(((const ImageCacheTile*)tile)->id());
    const SubimageInfo& si(id.file().subimageinfo(id.subimage()));
    const ImageDims& dims(si.leveldims(id.miplevel()));
    return ROI(id.x(), id.x() + dims.tile_width, id.y(),
               id.y() + dims.tile_height, id.z(), id.z() + dims.tile_depth,
               id.chbegin(), id.chend());
}



const void*
ImageCacheImpl::tile_pixels(ImageCache::Tile* tile, TypeDesc& format) const
{
    if (!tile)
        return NULL;
    ImageCacheTile* t = (ImageCacheTile*)tile;
    format            = t->file().datatype(t->id().subimage());
    return t->data();
}



bool
ImageCacheImpl::add_file(ustring filename, ImageInput::Creator creator,
                         const ImageSpec* config, bool replace)
{
    ImageCachePerThreadInfo* thread_info = get_perthread_info();
    ImageCacheFile* file = find_file(filename, thread_info, creator, config,
                                     replace);
    file                 = verify_file(file, thread_info);
    return file && !file->broken() && !file->is_udim();
}



bool
ImageCacheImpl::add_tile(ustring filename, int subimage, int miplevel, int x,
                         int y, int z, int chbegin, int chend, TypeDesc format,
                         const void* buffer, stride_t xstride, stride_t ystride,
                         stride_t zstride, bool copy)
{
    ImageCachePerThreadInfo* thread_info = get_perthread_info();
    ImageCacheFile* file                 = find_file(filename, thread_info);
    file                                 = verify_file(file, thread_info);
    if (!file || file->broken()) {
        if (!file || file->errors_should_issue())
            error(
                "Cannot add_tile for an image file that was not set up with add_file()");
        return false;
    }
    if (file->is_udim()) {
        error("Cannot add_tile to a UDIM-like virtual file");
        return false;
    }
    if (chend < chbegin) {  // chend < chbegin means "all channels."
        chbegin = 0;
        chend   = file->spec(subimage).nchannels;
    }
    TileID tileid(*file, subimage, miplevel, x, y, z, chbegin, chend);
    ImageCacheTileRef tile = new ImageCacheTile(tileid, buffer, format, xstride,
                                                ystride, zstride, copy);
    if (!tile || !tile->valid()) {
        if (file->errors_should_issue())
            error("Could not construct the tile; unknown reasons.");
        return false;
    }
    return add_tile_to_cache(tile, thread_info);
}



bool
ImageCacheImpl::add_tile(ustring filename, int subimage, int miplevel, int x,
                         int y, int z, int chbegin, int chend, TypeDesc format,
                         const image_span<const std::byte>& buffer, bool copy)
{
    ImageCachePerThreadInfo* thread_info = get_perthread_info();
    ImageCacheFile* file                 = find_file(filename, thread_info);
    file                                 = verify_file(file, thread_info);
    if (!file || file->broken()) {
        if (!file || file->errors_should_issue())
            error(
                "Cannot add_tile for an image file that was not set up with add_file()");
        return false;
    }
    if (file->is_udim()) {
        error("Cannot add_tile to a UDIM-like virtual file");
        return false;
    }
    if (chend < chbegin) {  // chend < chbegin means "all channels."
        chbegin = 0;
        chend   = file->spec(subimage).nchannels;
    }
    TileID tileid(*file, subimage, miplevel, x, y, z, chbegin, chend);
    ImageCacheTileRef tile
        = new ImageCacheTile(tileid, buffer.data(), format, buffer.xstride(),
                             buffer.ystride(), buffer.zstride(), copy);
    if (!tile || !tile->valid()) {
        if (file->errors_should_issue())
            error("Could not construct the tile; unknown reasons.");
        return false;
    }
    return add_tile_to_cache(tile, thread_info);
}



void
ImageCacheImpl::invalidate(ustring filename, bool force)
{
    ImageCacheFileRef file;
    {
        bool found = m_files.retrieve(filename, file);
        if (!found)
            return;  // no such file
    }

    invalidate(file.get(), force);
}



void
ImageCacheImpl::invalidate(ImageHandle* handle, bool force)
{
    ImageCacheFileRef file(handle);

    if (!force) {
        // If not in force mode, we don't do anything if the modification
        // time of the file has not changed since we opened it.
        recursive_timed_lock_guard guard(file->m_input_mutex);
        if (file->mod_time() == Filesystem::last_write_time(file->filename())
            && !file->broken())
            return;
    }

    // Iterate over the entire tilecache, record the TileID's of all
    // tiles that are from the file we are invalidating.
    std::vector<TileID> tiles_to_delete;
    for (TileCache::iterator tci = m_tilecache.begin(), e = m_tilecache.end();
         tci != e; ++tci) {
        if (&(*tci).second->file() == file)
            tiles_to_delete.push_back((*tci).second->id());
    }
    // N.B. at this point, we hold no locks!

    // Safely erase all the tiles we found
    for (const TileID& id : tiles_to_delete)
        m_tilecache.erase(id);

    const ustring fingerprint = file->fingerprint();

    // Invalidate the file itself (close it and clear its spec)
    file->invalidate();

    // Remove the fingerprint corresponding to this file
    {
        spin_lock lock(m_fingerprints_mutex);
        m_fingerprints.erase(fingerprint);
    }

    purge_perthread_microcaches();
}



void
ImageCacheImpl::invalidate_all(bool force)
{
    // Special case: invalidate EVERYTHING -- we can take some shortcuts
    // to do it all in one shot.
    if (force) {
        // Clear the whole tile cache
        m_tilecache.clear();
        // Invalidate (close and clear spec) all individual files
        for (FilenameMap::iterator fileit = m_files.begin(), e = m_files.end();
             fileit != e; ++fileit) {
            fileit->second->invalidate();
        }
        // Clear fingerprints list
        clear_fingerprints();
        // Mark the per-thread microcaches as invalid
        purge_perthread_microcaches();
        return;
    }

    // Not forced... we need to look for particular files that seem
    // to need invalidation.

    // Make a list of all files that need to be invalidated
    std::vector<ustring> all_files;
    for (FilenameMap::iterator fileit = m_files.begin(), e = m_files.end();
         fileit != e; ++fileit) {
        const ImageCacheFileRef& f(fileit->second);
        ustring name = f->filename();
        Timer input_mutex_timer;
        recursive_timed_lock_guard guard(f->m_input_mutex);
        f->m_mutex_wait_time += input_mutex_timer();
        // If the file was broken when we opened it, or if it no longer
        // exists, definitely invalidate it.
        if (f->broken() || !Filesystem::exists(name)) {
            all_files.push_back(name);
            continue;
        }
        // Invalidate the file if it has been modified since it was
        // last opened.
        std::time_t t = Filesystem::last_write_time(name);
        if (t != f->mod_time()) {
            all_files.push_back(name);
            continue;
        }
        for (int s = 0; s < f->subimages(); ++s) {
            const ImageCacheFile::SubimageInfo& sub(f->subimageinfo(s));
            // Invalidate if any unmipped subimage didn't automip but
            // automip is now on, or did automip but automip is now off.
            if (sub.unmipped
                && ((m_automip && f->miplevels(s) <= 1)
                    || (!m_automip && f->miplevels(s) > 1))) {
                all_files.push_back(name);
                break;
            }
            // Invalidate if any untiled subimage doesn't match the current
            // auto-tile setting.
            if (sub.untiled) {
                for (int m = 0, mend = f->miplevels(s); m < mend; ++m) {
                    const SubimageInfo& si(f->subimageinfo(s));
                    const ImageDims& dims(si.leveldims(m));
                    if (dims.tile_width != m_autotile
                        || dims.tile_height != m_autotile) {
                        all_files.push_back(name);
                        break;
                    }
                }
            }
        }
    }

    // Now, invalidate all the files in our "needs invalidation" list
    for (auto f : all_files) {
        // print(stderr, "Invalidating {}\n", f);
        invalidate(f, true);
    }

    // Mark the per-thread microcaches as invalid
    purge_perthread_microcaches();
}



void
ImageCacheImpl::close(ustring filename)
{
    auto f = m_files.find(filename);
    if (f != m_files.end())
        f->second->close();
}



void
ImageCacheImpl::close_all()
{
    for (auto& f : m_files)
        f.second->close();
}



void
ImageCacheFile::udim_setup()
{
    m_udim_nutiles = 0;  // assume it's not a udim at all
    m_udim_nvtiles = 0;

    // If it's a literal, existing file, always treat it as a regular
    // texture, even if it has what looks like udim pattern markers.
    if (Filesystem::exists(m_filename)) {
        return;
    }

    // Look for udim pattern markers, which identify this as a udim pattern.
    // We separately identify whether it's the kind of pattern that has a
    // single index, or separate u and v indices (and for separate, whether
    // we use the convention of adding 1 to the tile index) -- we're going
    // to need to know that later.
    // Note that we use `rcontains()` here for a little extra speed, knowing
    // that these markers will almost always be close to the end of the
    // full pathname.
    bool udim1   = (Strutil::rcontains(m_filename, "<UDIM>")
                  || Strutil::rcontains(m_filename, "%(UDIM)d"));
    bool udim2_0 = (Strutil::rcontains(m_filename, "<u>")
                    || Strutil::rcontains(m_filename, "<v>")
                    || Strutil::rcontains(m_filename, "<uvtile>")
                    || Strutil::rcontains(m_filename, "_u##v##"));
    bool udim2_1 = (Strutil::rcontains(m_filename, "<U>")
                    || Strutil::rcontains(m_filename, "<V>")
                    || Strutil::rcontains(m_filename, "<UVTILE>"));
    bool udim2   = udim2_0 | udim2_1;  // separate u and v indices
    bool is_udim = udim1 | udim2;
    if (!is_udim)
        return;

    // Enumerate all the matching textures by looking at all files in the
    // directory portion of the pattern, and seeing if they match a regex
    // we derive from the non-directory part of the pattern.
    std::vector<std::string> filenames;
    std::string dirname = Filesystem::parent_path(m_filename);
    if (dirname.empty())
        dirname = ".";
    std::string pat = udim_to_wildcard(Filesystem::filename(m_filename));
    Filesystem::get_directory_entries(dirname, filenames, false /*recurse*/,
                                      pat);

    // Now we have all the matching filenames, and we need to associate
    // these with uv tile numbering, for which we again use the regex
    // pattern, and our prior knowledge of whether it's a 1-number or
    // 2-number kind of pattern, to populate the lookup map.
    std::vector<UdimInfo> udim_list;  // temporary -- we don't know extent
    udim_list.reserve(100);
    std::regex decoder(pat);
    for (auto udim_tile_name : filenames) {
        std::string fnpart = Filesystem::filename(udim_tile_name);
        std::match_results<std::string::const_iterator> match;
        if (std::regex_match(fnpart, match, decoder) && match.size() > 1) {
            int u = Strutil::stoi(std::string(match[1].first, match[1].second));
            int v = 0;
            if (udim1) {
                // uv together -- disassemble it
                v = (u - 1001) / 10;
                u = (u - 1001) % 10;
            } else {
                // u and v separate, there's a 2nd number to match
                if (match.size() < 3)
                    continue;  // Did not completely match
                v = Strutil::stoi(std::string(match[2].first, match[2].second));
                if (udim2_1) {
                    u -= 1;
                    v -= 1;
                }
            }
            if (u < 0 || v < 0)
                continue;  // invalid tile
            udim_list.emplace_back(ustring(udim_tile_name), nullptr, u, v);
            m_udim_nutiles = std::max(m_udim_nutiles, short(u + 1));
            m_udim_nvtiles = std::max(m_udim_nvtiles, short(v + 1));
        }
    }

    // Now that we have the inventory, we can reassemble the udim info into
    // a simply indexed vector.
    m_udim_lookup.clear();
    m_udim_lookup.resize(int(m_udim_nutiles) * int(m_udim_nvtiles));
    for (auto& ud : udim_list) {
        m_udim_lookup[ud.v * m_udim_nutiles + ud.u] = ud;
    }
}



std::string
ImageCacheFile::udim_to_wildcard(string_view udimpattern)
{
    // dot, parens, brackets, and braces are special in regex and need to be
    // preceded by a backslash to be matched literally.
    std::string result = Filesystem::filename_to_regex(udimpattern);
    // Replace the udim specifiers with numeric wildcards, and surround the
    // number parts with regex parentheses, which will allow us later to
    // extract just those regions from the match.
    result = Strutil::replace(result, "<UDIM>", "([0-9]{4})", true);
    result = Strutil::replace(result, "<u>", "u([0-9]+)", true);
    result = Strutil::replace(result, "<v>", "v([0-9]+)", true);
    result = Strutil::replace(result, "<U>", "u([0-9]+)", true);
    result = Strutil::replace(result, "<V>", "v([0-9]+)", true);
    result = Strutil::replace(result, "_u##v##", "_u([0-9]+)v([0-9]+)", true);
    result = Strutil::replace(result, "<uvtile>", "u([0-9]+)_v([0-9]+)", true);
    result = Strutil::replace(result, "<UVTILE>", "u([0-9]+)_v([0-9]+)", true);
    // Ugh, "%(UDIM)d" is tricky because we already backslashed it!
    result = Strutil::replace(result, "%\\(UDIM\\)d", "([0-9]{4})", true);
    return result;
}



ImageCacheFile*
ImageCacheImpl::resolve_udim(ImageCacheFile* udimfile, Perthread* thread_info,
                             int utile, int vtile)
{
    if (!udimfile || !udimfile->is_udim())
        return udimfile;  // nothing to resolve;

    if (utile < 0 || utile >= udimfile->m_udim_nutiles || vtile < 0
        || vtile >= udimfile->m_udim_nvtiles)
        return nullptr;  // out of range

    // If udimfile exists, then we've already inventoried the matching
    // files and filled in udimfile->udim_lookup. That vector, and the
    // filename fields, are set and can be accessed without locks. The
    // `ImageCacheFile*` within it, however, should use rw lock access.
    int index = utile + vtile * udimfile->m_udim_nutiles;
    OIIO_DASSERT(index >= 0 && size_t(index) < udimfile->m_udim_lookup.size());
    UdimInfo& udiminfo(udimfile->m_udim_lookup[index]);

    // An empty filename in the record means that tile is not populated.
    if (udiminfo.filename.empty())
        return nullptr;

    ImageCacheFile* realfile = udiminfo.icfile;
    if (!realfile) {
        realfile        = find_file(udiminfo.filename, thread_info);
        udiminfo.icfile = realfile;
    }
    return realfile;
}



void
ImageCacheImpl::inventory_udim(ImageCacheFile* udimfile, Perthread* thread_info,
                               std::vector<ustring>& filenames, int& nutiles,
                               int& nvtiles)
{
    if (!udimfile || !udimfile->is_udim()) {
        filenames.clear();
        nutiles = 0;
        nvtiles = 0;
        return;
    }
    nutiles = udimfile->m_udim_nutiles;
    nvtiles = udimfile->m_udim_nvtiles;
    int n   = nutiles * nvtiles;
    filenames.resize(n);
    for (int i = 0; i < n; ++i) {
        const UdimInfo& udiminfo(udimfile->m_udim_lookup[i]);
        filenames[i] = udiminfo.filename;
    }
}



ImageCachePerThreadInfo*
ImageCacheImpl::create_thread_info()
{
    ImageCachePerThreadInfo* p = new ImageCachePerThreadInfo;
    // printf ("New perthread %p\n", (void *)p);
    spin_lock lock(m_perthread_info_mutex);
    m_all_perthread_info.emplace_back(p);
    return p;
}



void
ImageCacheImpl::destroy_thread_info(ImageCachePerThreadInfo* thread_info)
{
    if (!thread_info)
        return;
    // the ImageCache owns the thread_infos associated with it,
    // so all we need to do is find the entry and reset the unique pointer
    // to fully destroy the object
    spin_lock lock(m_perthread_info_mutex);
    for (auto& p : m_all_perthread_info) {
        if (p.get() == thread_info) {
            p.reset();
            break;
        }
    }
}



ImageCachePerThreadInfo*
ImageCacheImpl::get_perthread_info(ImageCachePerThreadInfo* p)
{
    if (!p) {
        // user has not provided an ImageCachePerThreadInfo yet
        ImageCachePerThreadInfo*& ptr
            = imagecache_per_thread_infos[imagecache_id];
        p = ptr;
        if (!p) {
            // this thread doesn't have a ImageCachePerThreadInfo for this ImageCacheImpl yet
            ptr = p = new ImageCachePerThreadInfo;
            // printf ("New perthread %p\n", (void *)p);
            spin_lock lock(m_perthread_info_mutex);
            m_all_perthread_info.emplace_back(p);
        }
    }
    if (p->purge) {  // has somebody requested a tile purge?
        // This is safe, because it's our thread.
        spin_lock lock(m_perthread_info_mutex);
        p->tile     = NULL;
        p->lasttile = NULL;
        p->purge    = 0;
        p->m_thread_files.clear();
    }
    return p;
}

void
ImageCacheImpl::purge_perthread_microcaches()
{
    // Mark the per-thread microcaches as invalid
    spin_lock lock(m_perthread_info_mutex);
    for (size_t i = 0, e = m_all_perthread_info.size(); i < e; ++i)
        if (m_all_perthread_info[i])
            m_all_perthread_info[i]->purge = 1;
}



bool
ImageCacheImpl::has_error() const
{
    auto iter = imcache_error_messages.find(imagecache_id);
    if (iter == imcache_error_messages.end())
        return false;
    return iter.value().size() > 0;
}



std::string
ImageCacheImpl::geterror(bool clear) const
{
    std::string e;
    auto iter = imcache_error_messages.find(imagecache_id);
    if (iter != imcache_error_messages.end()) {
        e = iter.value();
        if (clear)
            imcache_error_messages.erase(iter);
    }
    return e;
}



void
ImageCacheImpl::append_error(string_view message) const
{
    if (message.size() && message.back() == '\n')
        message.remove_suffix(1);
    std::string& err_str = imcache_error_messages[imagecache_id];
    OIIO_DASSERT(
        err_str.size() < 1024 * 1024 * 16
        && "Accumulated error messages > 16MB. Try checking return codes!");
    if (err_str.size() && err_str.back() != '\n')
        err_str += '\n';
    err_str.append(message.begin(), message.end());
}



size_t
ImageCacheImpl::heapsize() const
{
    using OIIO::pvt::footprint;
    using OIIO::pvt::heapsize;

    size_t size = 0;
    // strings
    size += heapsize(m_searchpath) + heapsize(m_plugin_searchpath)
            + heapsize(m_searchdirs);
    // thread info
    size += heapsize(m_all_perthread_info);
    // tile cache
    for (TileCache::iterator t = m_tilecache.begin(), e = m_tilecache.end();
         t != e; ++t)
        size += footprint(t->first) + footprint(t->second);
    // files
    for (FilenameMap::iterator t = m_files.begin(), e = m_files.end(); t != e;
         ++t)
        size += footprint(t->first) + footprint(t->second);
    // finger prints; we only account for references, this map does not own the files.
    constexpr size_t sizeofFingerprintPair = sizeof(ustring)
                                             + sizeof(ImageCacheFileRef);
    size += m_fingerprints.size() * sizeofFingerprintPair;
    return size;
}



size_t
ImageCacheImpl::footprint(ImageCacheFootprint& output) const
{
    using OIIO::pvt::footprint;
    using OIIO::pvt::heapsize;

    // strings
    output.ic_str_count = m_searchdirs.size() + 2;
    output.ic_str_mem   = heapsize(m_searchdirs) + heapsize(m_searchpath)
                        + heapsize(m_plugin_searchpath);

    // thread info
    output.ic_thdi_count = m_all_perthread_info.size();
    output.ic_thdi_mem   = heapsize(m_all_perthread_info);

    // tile cache
    output.ic_tile_count = m_tilecache.size();
    for (TileCache::iterator t = m_tilecache.begin(), e = m_tilecache.end();
         t != e; ++t)
        output.ic_tile_mem += footprint(t->first) + footprint(t->second);

    // finger prints; we only account for references, this map does not own the files.
    constexpr size_t sizeofFingerprintPair = sizeof(ustring)
                                             + sizeof(ImageCacheFileRef);
    output.ic_fgpt_count = m_fingerprints.size();
    output.ic_fgpt_mem   = output.ic_fgpt_count * sizeofFingerprintPair;

    // files; count the footprint of files, subimages, level infos, image inputs, image specs
    for (FilenameMap::iterator t = m_files.begin(), e = m_files.end(); t != e;
         ++t) {
        // get file format ustring; files with empty file format are simply constant valued.
        const ImageCacheFile& file(*t->second);
        const ustring& format = !file.fileformat().empty()
                                    ? file.fileformat()
                                    : ImageCacheFootprint::uconstant;

        const size_t fileftp = footprint(t->first) + footprint(t->second);
        output.add<kMem>(fileftp, format);

        const size_t specftp = footprint(file.m_configspec);
        output.add<kSpecMem>(specftp, format);

        for (const auto& sptr : file.m_pool_specs)
            output.add<kSpecMem>(footprint(sptr), format);

        for (const auto& sptr : file.m_pool_dims)
            output.add<kSpecMem>(footprint(sptr), format);

        const size_t inputftp = footprint(file.m_input);
        output.add<kInputMem>(inputftp, format);

        // subimages
        for (int s = 0, send = file.subimages(); s < send; ++s) {
            const ImageCacheFile::SubimageInfo& sub(file.subimageinfo(s));
            const size_t subftp = footprint(sub);
            output.add<kSubImageMem>(subftp, format);

            // level infos
            for (const auto& level : sub.levels) {
                const size_t lvlftp = footprint(level);
                output.add<kLevelInfoMem>(lvlftp, format);
            }
        }
    }

    // update total memory
    output.ic_mem += output.ic_str_mem;
    output.ic_mem += output.ic_tile_mem;
    output.ic_mem += output.ic_thdi_mem;
    output.ic_mem += output.fmap.find(ImageCacheFootprint::utotal)->second[kMem];
    output.ic_mem += output.ic_fgpt_mem;

    return output.ic_mem;
}



void
ImageCache::impl_deleter(ImageCacheImpl* todel)
{
    delete todel;
}



ImageCache::ImageCache()
    : m_impl(new ImageCacheImpl, &impl_deleter)
{
}



ImageCache::~ImageCache() {}



std::shared_ptr<ImageCache>
ImageCache::create(bool shared)
{
    if (shared) {
        // They requested a shared cache.  If a shared cache already
        // exists, just return it, otherwise record the new cache.
        spin_lock guard(shared_image_cache_mutex);
        if (!shared_image_cache)
            shared_image_cache = std::make_shared<ImageCache>();
        return shared_image_cache;
    }

    // Doesn't need a shared cache
    return std::make_shared<ImageCache>();
}



void
ImageCache::destroy(std::shared_ptr<ImageCache>& cache, bool teardown)
{
    if (!cache)
        return;
    spin_lock guard(shared_image_cache_mutex);
    if (cache.get() == shared_image_cache.get()) {
        // This is the shared cache, so don't really delete it. Invalidate
        // it fully, closing the files and throwing out any tiles that
        // nobody is currently holding references to.  But only delete the
        // IC fully if 'teardown' is true, and even then, it won't destroy
        // until nobody else is still holding a shared_ptr to it.
        cache->invalidate_all(teardown);
        if (teardown)
            shared_image_cache.reset();
    }
    cache.reset();
}



bool
ImageCache::attribute(string_view name, TypeDesc type, const void* val)
{
    return m_impl->attribute(name, type, val);
}



bool
ImageCache::getattribute(string_view name, TypeDesc type, void* val) const
{
    return m_impl->getattribute(name, type, val);
}


TypeDesc
ImageCache::getattributetype(string_view name) const
{
    return m_impl->getattributetype(name);
}



ImageCache::Perthread*
ImageCache::get_perthread_info(Perthread* thread_info)
{
    return m_impl->get_perthread_info(thread_info);
}



ImageCache::Perthread*
ImageCache::create_thread_info()
{
    return m_impl->create_thread_info();
}



void
ImageCache::destroy_thread_info(Perthread* thread_info)
{
    m_impl->destroy_thread_info(thread_info);
}



ImageCache::ImageHandle*
ImageCache::get_image_handle(ustring filename, Perthread* thread_info,
                             const TextureOpt* options)
{
    return m_impl->get_image_handle(filename, thread_info, options);
}



bool
ImageCache::good(ImageHandle* file)
{
    return m_impl->good(file);
}



ustring
ImageCache::filename_from_handle(ImageHandle* handle)
{
    return m_impl->filename_from_handle(handle);
}



std::string
ImageCache::resolve_filename(const std::string& filename) const
{
    return m_impl->resolve_filename(filename);
}



bool
ImageCache::get_image_info(ustring filename, int subimage, int miplevel,
                           ustring dataname, TypeDesc datatype, void* data)
{
    return m_impl->get_image_info(filename, subimage, miplevel, dataname,
                                  datatype, data);
}



bool
ImageCache::get_image_info(ImageHandle* file, Perthread* thread_info,
                           int subimage, int miplevel, ustring dataname,
                           TypeDesc datatype, void* data)
{
    return m_impl->get_image_info(file, thread_info, subimage, miplevel,
                                  dataname, datatype, data);
}



bool
ImageCache::get_imagespec(ustring filename, ImageSpec& spec, int subimage)
{
    return m_impl->get_imagespec(filename, spec, subimage);
}


bool
ImageCache::get_imagespec(ImageHandle* file, Perthread* thread_info,
                          ImageSpec& spec, int subimage)
{
    return m_impl->get_imagespec(file, thread_info, spec, subimage);
}



const ImageSpec*
ImageCache::imagespec(ustring filename, int subimage)
{
    return m_impl->imagespec(filename, subimage);
}


const ImageSpec*
ImageCache::imagespec(ImageHandle* file, Perthread* thread_info, int subimage)
{
    return m_impl->imagespec(file, thread_info, subimage);
}



bool
ImageCache::get_cache_dimensions(ustring filename, ImageSpec& spec,
                                 int subimage, int miplevel)
{
    return m_impl->get_cache_dimensions(filename, spec, subimage, miplevel);
}


bool
ImageCache::get_cache_dimensions(ImageHandle* file, Perthread* thread_info,
                                 ImageSpec& spec, int subimage, int miplevel)
{
    return m_impl->get_cache_dimensions(file, thread_info, spec, subimage,
                                        miplevel);
}



bool
ImageCache::get_thumbnail(ustring filename, ImageBuf& thumbnail, int subimage)
{
    return m_impl->get_thumbnail(filename, thumbnail, subimage);
}


bool
ImageCache::get_thumbnail(ImageHandle* file, Perthread* thread_info,
                          ImageBuf& thumbnail, int subimage)
{
    return m_impl->get_thumbnail(file, thread_info, thumbnail, subimage);
}



bool
ImageCache::get_pixels(ustring filename, int subimage, int miplevel, int xbegin,
                       int xend, int ybegin, int yend, int zbegin, int zend,
                       int chbegin, int chend, TypeDesc format, void* result,
                       stride_t xstride, stride_t ystride, stride_t zstride,
                       int cache_chbegin, int cache_chend)
{
    return m_impl->get_pixels(filename, subimage, miplevel, xbegin, xend,
                              ybegin, yend, zbegin, zend, chbegin, chend,
                              format, result, xstride, ystride, zstride,
                              cache_chbegin, cache_chend);
}


bool
ImageCache::get_pixels(ImageHandle* file, Perthread* thread_info, int subimage,
                       int miplevel, int xbegin, int xend, int ybegin, int yend,
                       int zbegin, int zend, int chbegin, int chend,
                       TypeDesc format, void* result, stride_t xstride,
                       stride_t ystride, stride_t zstride, int cache_chbegin,
                       int cache_chend)
{
    return m_impl->get_pixels(file, thread_info, subimage, miplevel, xbegin,
                              xend, ybegin, yend, zbegin, zend, chbegin, chend,
                              format, result, xstride, ystride, zstride,
                              cache_chbegin, cache_chend);
}



bool
ImageCache::get_pixels(ustring filename, int subimage, int miplevel, int xbegin,
                       int xend, int ybegin, int yend, int zbegin, int zend,
                       TypeDesc format, void* result)
{
    return m_impl->get_pixels(filename, subimage, miplevel, xbegin, xend,
                              ybegin, yend, zbegin, zend, format, result);
}


bool
ImageCache::get_pixels(ImageHandle* file, Perthread* thread_info, int subimage,
                       int miplevel, int xbegin, int xend, int ybegin, int yend,
                       int zbegin, int zend, TypeDesc format, void* result)
{
    return m_impl->get_pixels(file, thread_info, subimage, miplevel, xbegin,
                              xend, ybegin, yend, zbegin, zend, format, result);
}



void
ImageCache::invalidate(ustring filename, bool force)
{
    m_impl->invalidate(filename, force);
}


void
ImageCache::invalidate(ImageHandle* file, bool force)
{
    m_impl->invalidate(file, force);
}


void
ImageCache::invalidate_all(bool force)
{
    m_impl->invalidate_all(force);
}


void
ImageCache::close(ustring filename)
{
    m_impl->close(filename);
}


void
ImageCache::close_all()
{
    m_impl->close_all();
}


ImageCache::Tile*
ImageCache::get_tile(ustring filename, int subimage, int miplevel, int x, int y,
                     int z, int chbegin, int chend)
{
    return m_impl->get_tile(filename, subimage, miplevel, x, y, z, chbegin,
                            chend);
}


ImageCache::Tile*
ImageCache::get_tile(ImageHandle* file, Perthread* thread_info, int subimage,
                     int miplevel, int x, int y, int z, int chbegin, int chend)
{
    return m_impl->get_tile(file, thread_info, subimage, miplevel, x, y, z,
                            chbegin, chend);
}



void
ImageCache::release_tile(Tile* tile) const
{
    m_impl->release_tile(tile);
}



TypeDesc
ImageCache::tile_format(const Tile* tile) const
{
    return m_impl->tile_format(tile);
}



ROI
ImageCache::tile_roi(const Tile* tile) const
{
    return m_impl->tile_roi(tile);
}



const void*
ImageCache::tile_pixels(Tile* tile, TypeDesc& format) const
{
    return m_impl->tile_pixels(tile, format);
}



bool
ImageCache::add_file(ustring filename, ImageInput::Creator creator,
                     const ImageSpec* config, bool replace)
{
    return m_impl->add_file(filename, creator, config, replace);
}



bool
ImageCache::add_tile(ustring filename, int subimage, int miplevel, int x, int y,
                     int z, int chbegin, int chend, TypeDesc format,
                     const void* buffer, stride_t xstride, stride_t ystride,
                     stride_t zstride, bool copy)
{
    return m_impl->add_tile(filename, subimage, miplevel, x, y, z, chbegin,
                            chend, format, buffer, xstride, ystride, zstride,
                            copy);
}



bool
ImageCache::add_tile(ustring filename, int subimage, int miplevel, int x, int y,
                     int z, int chbegin, int chend, TypeDesc format,
                     const image_span<const std::byte>& buffer, bool copy)
{
    return m_impl->add_tile(filename, subimage, miplevel, x, y, z, chbegin,
                            chend, format, buffer, copy);
}



bool
ImageCache::has_error() const
{
    return m_impl->has_error();
}



std::string
ImageCache::geterror(bool clear) const
{
    return m_impl->geterror(clear);
}



std::string
ImageCache::getstats(int level) const
{
    return m_impl->getstats(level);
}



void
ImageCache::reset_stats()
{
    m_impl->reset_stats();
}


OIIO_NAMESPACE_END
