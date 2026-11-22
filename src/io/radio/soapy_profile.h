// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_IO_RADIO_SOAPY_PROFILE_H
#define DSD_NEO_IO_RADIO_SOAPY_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace dsdneo {

enum class SoapyProfileId : uint8_t {
    Auto = 0,
    Generic,
    Airspy,
    Sdrplay,
    Hackrf,
    Lime,
    Pluto,
    Rtlsdr,
    Uhd,
    Sddc,
};

enum class SoapyStreamFormat : uint8_t {
    Auto = 0,
    CF32,
    CS16,
};

enum class SoapySettingScope : uint8_t {
    Device = 0,
    Rx0,
};

enum class SoapySettingValueType : uint8_t {
    Bool = 0,
    Int,
    Float,
    String,
};

struct SoapyRange {
    double minimum;
    double maximum;
    double step;
};

struct SoapyProfile {
    SoapyProfileId id;
    const char* name;
    const char* display_name;
    double default_bandwidth_hz;
    /* RX antenna to select when the user configured none and the tuned frequency is above the
       HF/VHF split. NULL leaves the driver default in place. */
    const char* default_antenna;
    /* Sample rate to run when the device supports it exactly (soapy_profile_sample_rate()), whatever rate the stream
       asks for; 0 takes the device rate nearest the request. */
    double preferred_sample_rate_hz;
};

struct SoapyProfileSelection {
    std::string requested_profile;
    std::string driver_key;
    std::string hardware_key;
    std::string args;
};

struct SoapyBandwidthChoice {
    int bandwidth_hz;
    bool should_apply;
    bool explicit_request;
};

struct SoapySettingRequest {
    SoapySettingScope scope = SoapySettingScope::Device;
    std::string key;
    std::string value;
};

struct SoapyAntennaChoice {
    std::string name;           /* empty leaves the driver default in place */
    bool auto_selected = false; /* true when the profile default was applied */
};

/* Frequency above which a profile default antenna is applied. Devices such as the RX-888 family
   expose separate HF (direct sampling) and VHF (tuner) ports and start on the HF port. */
constexpr double kSoapyHfVhfSplitHz = 32000000.0;

const SoapyProfile& soapy_profile_by_id(SoapyProfileId id);
bool soapy_profile_parse_name(const std::string& value, SoapyProfileId* out_id);
SoapyProfileId soapy_select_profile_id(const SoapyProfileSelection& selection);

/* What a SoapySDR identity string (a driver key, a hardware key, the device arguments) says about the device, as bits:
   SOAPY_IDENTITY_AIRSPY names an Airspy, SOAPY_IDENTITY_AIRSPY_HF an Airspy HF+ (its keys hold "airspy" but it is not
   one), SOAPY_IDENTITY_OTHER another device the profile detection knows; 0 says nothing (empty, or opaque such as a
   proxy's "remote"). */
enum : uint8_t {
    SOAPY_IDENTITY_AIRSPY = 1,
    SOAPY_IDENTITY_AIRSPY_HF = 2,
    SOAPY_IDENTITY_OTHER = 4,
};

uint8_t soapy_identity_evidence(const std::string& text);

/* The device an opened SoapySDR handle is, from every identity bit its lookups gave (soapy_identity_evidence() of each
   key and the arguments, or-ed over every lookup so a later failed or empty one takes nothing away): an Airspy as soon
   as anything named one (a conflicting pair such as an RTL-SDR driver key with an Airspy hardware key included: the
   Airspy is the one that cannot be ruled out); else not an Airspy when anything named another device or an HF+; else
   unknown. */
enum class SoapyReceiver : uint8_t { NotAirspy = 0, Airspy = 1, Unknown = 2 };
SoapyReceiver soapy_receiver_from_evidence(uint8_t evidence);

bool soapy_stream_format_parse_name(const std::string& value, SoapyStreamFormat* out_format);
const char* soapy_stream_format_name(SoapyStreamFormat format);
std::string soapy_choose_stream_format(const std::vector<std::string>& supported_formats,
                                       const std::string& requested_format, const std::string& native_format);

/* SoapySDR leaves CS16 samples as the driver's integers. A driver whose native format is CS16 reports their full
 * scale (getNativeStreamFormat(); Pluto and bladeRF 2048, for 12-bit samples LSB aligned), and CS16 that is not
 * native is converted at 32768, as SoapySDR's converters and SoapyRemote assume. */
constexpr double kSoapyCs16DefaultFullScale = 32768.0;
/* The full scale a CS16 stream is read against, chosen once per stream format choice (the reader divides by it): the
 * driver's @p native_full_scale when @p stream_format and @p native_format are both CS16 and it is finite and in
 * [1, 32768]; else kSoapyCs16DefaultFullScale. */
double soapy_cs16_full_scale(const std::string& stream_format, const std::string& native_format,
                             double native_full_scale);

bool soapy_name_list_contains(const std::vector<std::string>& names, const std::string& wanted);
SoapyAntennaChoice soapy_choose_antenna(const char* profile_default_antenna, const std::string& configured_antenna,
                                        double center_freq_hz);
SoapyBandwidthChoice soapy_choose_bandwidth_hz(int tuner_bw_hz, bool tuner_bw_hz_is_set, int soapy_bandwidth_hz,
                                               double profile_default_bandwidth_hz);
double soapy_nearest_in_ranges(double requested, const std::vector<SoapyRange>& ranges, bool* out_supported);
double soapy_nearest_sample_rate(double requested, const std::vector<double>& listed_rates,
                                 const std::vector<SoapyRange>& ranges, bool* out_adjusted);
/* The advertised rate nearest @p requested among those not above @p max_rate in whole hertz (a rate that rounds to it,
 * as llround() programs and reads it back, is within it): each listed rate at or below it and each range's nearest
 * point once the range is cut back to its last point at or below it, compared with one another (a listed rate wins a
 * tie). A device that advertises no rate at all is given @p requested. Sets @p out_found (required) false, and returns
 * 0, when nothing advertised is at or below @p max_rate. Each rate it returns is an advertised point, so a write of it
 * (soapy_nearest_sample_rate() on every advertised rate) keeps it. */
double soapy_nearest_sample_rate_within(double requested, double max_rate, const std::vector<double>& listed_rates,
                                        const std::vector<SoapyRange>& ranges, bool* out_found);
/* Whether the device runs @p rate_hz itself: a listed rate within 1 Hz of it, or a range point (minimum + k * step,
 * any point of a range with no step) within 1 Hz. */
bool soapy_rate_supported_exactly(double rate_hz, const std::vector<double>& listed_rates,
                                  const std::vector<SoapyRange>& ranges);
/* The rate a device with @p profile is planned at for @p requested: the advertised point that matches the profile's
 * preferred rate when the device supports it exactly, else soapy_nearest_sample_rate(). @p out_adjusted as there. Only
 * planning uses it; a rate write programs the advertised rate nearest the rate the plan chose (or a rollback restores),
 * so a rate the stream asks for is never replaced by the preference. */
double soapy_profile_sample_rate(const SoapyProfile& profile, double requested, const std::vector<double>& listed_rates,
                                 const std::vector<SoapyRange>& ranges, bool* out_adjusted);

/* A driver whose receive queue dsd-neo bounds at setupStream(), so a retune's mute can swallow all of it. */
struct SoapyStreamQueue {
    const char* arg_key;   /* the stream argument that sets the queue depth */
    const char* arg_value; /* the depth dsd-neo asks for */
    /* What can still reach the mute from before a retune: that queue, its transfers in flight, and the block a reader
       paused between its read and the mute holds (one MTU, a buffer). */
    size_t backlog_samples;
};

/* The receive queue of the driver keyed @p driver_key (getDriverKey(), any case) in @p out, or false for a driver whose
 * queue dsd-neo does not set. SoapyHackRF queues 15 buffers of 131,072 samples unless told otherwise ("buffers"), and
 * libhackrf keeps 4 more in flight: about 249 ms at 10 MS/s, longer than the 120 ms retune mute. dsd-neo asks for 4,
 * which with the reader's block leaves 9 buffers (118 ms at 10 MS/s). */
bool soapy_driver_stream_queue(const std::string& driver_key, SoapyStreamQueue* out);

/* The narrowest tuner filter that keeps the channel whole at every DSP bandwidth while the capture runs the Fs/4
 * shift at @p capture_rate_hz: the channel sits a quarter of the rate off centre, so the filter's full width must
 * reach half the rate plus the widest DSP bandwidth (72 kHz). */
double soapy_fs4_channel_floor_hz(double capture_rate_hz);
/* The narrowest bandwidth @p ranges admit at or above @p floor_hz (a range's steps rounded up; one within 0.5 Hz below
 * counts), or the widest they admit when none reaches it; @p floor_hz when @p ranges admit none. Always a point a range
 * holds: a step, or for a range with no step a value within its ends. */
double soapy_floor_bandwidth(double floor_hz, const std::vector<SoapyRange>& ranges);
/* Whether a filter of @p applied_hz is narrower than soapy_fs4_channel_floor_hz() at @p capture_rate_hz. */
bool soapy_bandwidth_cuts_offset(double applied_hz, double capture_rate_hz);
std::string soapy_join_names(const std::vector<std::string>& names, size_t max_chars);
const char* soapy_setting_scope_name(SoapySettingScope scope);
bool soapy_parse_settings(const std::string& spec, std::vector<SoapySettingRequest>* out_requests,
                          std::string* out_error);
bool soapy_validate_setting_value(SoapySettingValueType type, const SoapyRange* range, const std::string& value,
                                  std::string* out_error);

} // namespace dsdneo

#endif /* DSD_NEO_IO_RADIO_SOAPY_PROFILE_H */
