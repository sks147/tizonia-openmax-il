/**
 * Copyright (C) 2011-2014 Aratelia Limited - Juan A. Rubio
 *
 * This file is part of Tizonia
 *
 * Tizonia is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Tizonia is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Tizonia.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file   tizvorbisgraph.cc
 * @author Juan A. Rubio <juan.rubio@aratelia.com>
 *
 * @brief  OpenMAX IL vorbis decoder graph implementation
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <boost/bind.hpp>
#include <boost/make_shared.hpp>

#include <OMX_Core.h>
#include <OMX_Component.h>
#include <tizosal.h>

#include "tizgraphutil.h"
#include "tizgraphconfig.h"
#include "tizprobe.h"
#include "tizvorbisgraph.h"

#ifdef TIZ_LOG_CATEGORY_NAME
#undef TIZ_LOG_CATEGORY_NAME
#define TIZ_LOG_CATEGORY_NAME "tiz.play.graph.vorbis"
#endif

namespace graph = tiz::graph;

graph::vorbisdecoder::vorbisdecoder () : graph::graph ("tizvorbisgraph")
{
}

graph::ops *graph::vorbisdecoder::do_init ()
{
  omx_comp_name_lst_t comp_list;
  comp_list.push_back ("OMX.Aratelia.container_demuxer.ogg");
  comp_list.push_back ("OMX.Aratelia.audio_decoder.vorbis");
  comp_list.push_back ("OMX.Aratelia.audio_renderer_nb.pcm");

  omx_comp_role_lst_t role_list;
  role_list.push_back ("container_demuxer.ogg");
  role_list.push_back ("audio_decoder.vorbis");
  role_list.push_back ("audio_renderer.pcm");

  return new vorbisdecops (this, comp_list, role_list);
}

//
// vorbisdecops
//
graph::vorbisdecops::vorbisdecops (graph *p_graph,
                                   const omx_comp_name_lst_t &comp_lst,
                                   const omx_comp_role_lst_t &role_lst)
  : tiz::graph::ops (p_graph, comp_lst, role_lst),
    need_port_settings_changed_evt_ (false)
{
}

void graph::vorbisdecops::do_disable_ports ()
{
  OMX_U32 demuxers_video_port = 1;
  G_OPS_BAIL_IF_ERROR (util::disable_port (handles_[0], demuxers_video_port),
                       "Unable to disable demuxer's video port.");
  clear_expected_port_transitions ();
  add_expected_port_transition (handles_[0], demuxers_video_port,
                                OMX_CommandPortDisable);
}

void graph::vorbisdecops::do_probe ()
{
  TIZ_LOG (TIZ_PRIORITY_TRACE, "current_file_index_ [%d]...",
           current_file_index_);
  assert (current_file_index_ < file_list_.size ());
  G_OPS_BAIL_IF_ERROR (probe_uri (current_file_index_), "Unable to probe uri.");
  G_OPS_BAIL_IF_ERROR (set_vorbis_settings (),
                       "Unable to set OMX_IndexParamAudioVorbis");
}

bool graph::vorbisdecops::is_port_settings_evt_required () const
{
  return need_port_settings_changed_evt_;
}

bool graph::vorbisdecops::is_disabled_evt_required () const
{
  return true;
}

void graph::vorbisdecops::do_configure ()
{
  G_OPS_BAIL_IF_ERROR (
      tiz::graph::util::set_content_uri (handles_[0], probe_ptr_->get_uri ()),
      "Unable to set OMX_IndexParamContentURI");
  G_OPS_BAIL_IF_ERROR (
      tiz::graph::util::set_pcm_mode (
          handles_[2], 0,
          boost::bind (&tiz::probe::get_pcm_codec_info, probe_ptr_, _1)),
      "Unable to set OMX_IndexParamAudioPcm");
}

OMX_ERRORTYPE
graph::vorbisdecops::probe_uri (const int uri_index, const bool quiet)
{
  assert (uri_index < file_list_.size ());

  const std::string &uri = file_list_[uri_index];

  if (!uri.empty ())
  {
    // Probe a new uri
    probe_ptr_.reset ();
    bool quiet_probing = true;
    probe_ptr_ = boost::make_shared<tiz::probe>(uri, quiet_probing);
    if (probe_ptr_->get_omx_domain () != OMX_PortDomainAudio
        || probe_ptr_->get_audio_coding_type () != OMX_AUDIO_CodingVORBIS)
    {
      return OMX_ErrorContentURIError;
    }
    if (!quiet)
    {
      tiz::graph::util::dump_graph_info ("vorbis", "decode", uri);
      probe_ptr_->dump_stream_metadata ();
      probe_ptr_->dump_pcm_info ();
    }
  }
  return OMX_ErrorNone;
}

OMX_ERRORTYPE
graph::vorbisdecops::set_vorbis_settings ()
{
  // Retrieve the current vorbis settings from the decoder's port #0
  OMX_AUDIO_PARAM_VORBISTYPE vorbistype_orig;
  TIZ_INIT_OMX_PORT_STRUCT (vorbistype_orig, 0 /* port id */);

  tiz_check_omx_err (OMX_GetParameter (handles_[1], OMX_IndexParamAudioVorbis,
                                       &vorbistype_orig));

  // Set the vorbis settings on decoder's port #0
  OMX_AUDIO_PARAM_VORBISTYPE vorbistype;
  TIZ_INIT_OMX_PORT_STRUCT (vorbistype, 0 /* port id */);

  probe_ptr_->get_vorbis_codec_info (vorbistype);
  vorbistype.nPortIndex = 0;
  tiz_check_omx_err (
      OMX_SetParameter (handles_[1], OMX_IndexParamAudioVorbis, &vorbistype));

  // Record whether we need to wait for a port settings change event or not
  // (the decoder output port implements the "slaving" behaviour)
  if (vorbistype_orig.nSampleRate != vorbistype.nSampleRate
      || vorbistype_orig.nChannels != vorbistype.nChannels)
  {
    need_port_settings_changed_evt_ = true;
  }
  else
  {
    need_port_settings_changed_evt_ = false;
  }

  return OMX_ErrorNone;
}