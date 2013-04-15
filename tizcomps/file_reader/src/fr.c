/**
 * Copyright (C) 2011-2013 Aratelia Limited - Juan A. Rubio
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
 * @file   fr.c
 * @author Juan A. Rubio <juan.rubio@aratelia.com>
 *
 * @brief  Tizonia OpenMAX IL - Binary Reader Component
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <string.h>

#include "OMX_Core.h"
#include "OMX_Component.h"
#include "OMX_Types.h"

#include "tizosal.h"
#include "tizscheduler.h"
#include "tizbinaryport.h"
#include "frcfgport.h"
#include "frprc.h"

#ifdef TIZ_LOG_CATEGORY_NAME
#undef TIZ_LOG_CATEGORY_NAME
#define TIZ_LOG_CATEGORY_NAME "tiz.file_reader"
#endif

#define ARATELIA_FILE_READER_AUDIO_READER_ROLE "audio_reader.binary"
#define ARATELIA_FILE_READER_VIDEO_READER_ROLE "video_reader.binary"
#define ARATELIA_FILE_READER_IMAGE_READER_ROLE "image_reader.binary"
#define ARATELIA_FILE_READER_OTHER_READER_ROLE "other_reader.binary"
#define ARATELIA_FILE_READER_COMPONENT_NAME "OMX.Aratelia.file_reader.binary"
#define ARATELIA_FILE_READER_PORT_MIN_BUF_COUNT 2
#define ARATELIA_FILE_READER_PORT_MIN_BUF_SIZE 1024
#define ARATELIA_FILE_READER_PORT_NONCONTIGUOUS OMX_FALSE
#define ARATELIA_FILE_READER_PORT_ALIGNMENT 0
#define ARATELIA_FILE_READER_PORT_SUPPLIERPREF OMX_BufferSupplyInput

static OMX_VERSIONTYPE file_reader_version = { {1, 0, 0, 0} };

static OMX_PTR
instantiate_audio_port (OMX_HANDLETYPE ap_hdl)
{
  OMX_PTR p_binaryport = NULL;
  tiz_port_options_t port_opts = {
    OMX_PortDomainAudio,
    OMX_DirOutput,
    ARATELIA_FILE_READER_PORT_MIN_BUF_COUNT,
    ARATELIA_FILE_READER_PORT_MIN_BUF_SIZE,
    ARATELIA_FILE_READER_PORT_NONCONTIGUOUS,
    ARATELIA_FILE_READER_PORT_ALIGNMENT,
    ARATELIA_FILE_READER_PORT_SUPPLIERPREF,
    {NULL, NULL, NULL},
    -1                          /* use -1 for now */
  };

  tiz_binaryport_init ();
  p_binaryport = factory_new (tizbinaryport, &port_opts);
  assert (p_binaryport);

  return p_binaryport;
}

static OMX_PTR
instantiate_video_port (OMX_HANDLETYPE ap_hdl)
{
  OMX_PTR p_binaryport = NULL;
  tiz_port_options_t port_opts = {
    OMX_PortDomainVideo,
    OMX_DirOutput,
    ARATELIA_FILE_READER_PORT_MIN_BUF_COUNT,
    ARATELIA_FILE_READER_PORT_MIN_BUF_SIZE,
    ARATELIA_FILE_READER_PORT_NONCONTIGUOUS,
    ARATELIA_FILE_READER_PORT_ALIGNMENT,
    ARATELIA_FILE_READER_PORT_SUPPLIERPREF,
    {NULL, NULL, NULL},
    -1                          /* use -1 for now */
  };

  tiz_binaryport_init ();
  p_binaryport = factory_new (tizbinaryport, &port_opts);
  assert (p_binaryport);

  return p_binaryport;
}

static OMX_PTR
instantiate_image_port (OMX_HANDLETYPE ap_hdl)
{
  OMX_PTR p_binaryport = NULL;
  tiz_port_options_t port_opts = {
    OMX_PortDomainImage,
    OMX_DirOutput,
    ARATELIA_FILE_READER_PORT_MIN_BUF_COUNT,
    ARATELIA_FILE_READER_PORT_MIN_BUF_SIZE,
    ARATELIA_FILE_READER_PORT_NONCONTIGUOUS,
    ARATELIA_FILE_READER_PORT_ALIGNMENT,
    ARATELIA_FILE_READER_PORT_SUPPLIERPREF,
    {NULL, NULL, NULL},
    -1                          /* use -1 for now */
  };

  tiz_binaryport_init ();
  p_binaryport = factory_new (tizbinaryport, &port_opts);
  assert (p_binaryport);

  return p_binaryport;
}

static OMX_PTR
instantiate_other_port (OMX_HANDLETYPE ap_hdl)
{
  OMX_PTR p_binaryport = NULL;
  tiz_port_options_t port_opts = {
    OMX_PortDomainOther,
    OMX_DirOutput,
    ARATELIA_FILE_READER_PORT_MIN_BUF_COUNT,
    ARATELIA_FILE_READER_PORT_MIN_BUF_SIZE,
    ARATELIA_FILE_READER_PORT_NONCONTIGUOUS,
    ARATELIA_FILE_READER_PORT_ALIGNMENT,
    ARATELIA_FILE_READER_PORT_SUPPLIERPREF,
    {NULL, NULL, NULL},
    -1                          /* use -1 for now */
  };

  tiz_binaryport_init ();
  p_binaryport = factory_new (tizbinaryport, &port_opts);
  assert (p_binaryport);

  return p_binaryport;
}

static OMX_PTR
instantiate_config_port (OMX_HANDLETYPE ap_hdl)
{
  OMX_PTR p_cport = NULL;

  /* Instantiate the config port */
  fr_cfgport_init ();
  p_cport = factory_new (frcfgport, NULL,       /* this port does not take options */
                         ARATELIA_FILE_READER_COMPONENT_NAME,
                         file_reader_version);
  assert (p_cport);

  return p_cport;
}

static OMX_PTR
instantiate_processor (OMX_HANDLETYPE ap_hdl)
{
  OMX_PTR p_proc = NULL;

  /* Instantiate the processor */
  fr_prc_init ();
  p_proc = factory_new (frprc, ap_hdl);
  assert (p_proc);

  return p_proc;
}

OMX_ERRORTYPE
OMX_ComponentInit (OMX_HANDLETYPE ap_hdl)
{
  tiz_role_factory_t audio_role;
  tiz_role_factory_t video_role;
  tiz_role_factory_t image_role;
  tiz_role_factory_t other_role;
  const tiz_role_factory_t *rf_list[] = { &audio_role, &video_role,
    &image_role, &other_role
  };

  assert (ap_hdl);

  TIZ_LOG (TIZ_TRACE, "OMX_ComponentInit: Inititializing [%s]",
           ARATELIA_FILE_READER_COMPONENT_NAME);

  strcpy ((OMX_STRING) audio_role.role,
          ARATELIA_FILE_READER_AUDIO_READER_ROLE);
  audio_role.pf_cport = instantiate_config_port;
  audio_role.pf_port[0] = instantiate_audio_port;
  audio_role.nports = 1;
  audio_role.pf_proc = instantiate_processor;

  strcpy ((OMX_STRING) video_role.role,
          ARATELIA_FILE_READER_VIDEO_READER_ROLE);
  video_role.pf_cport = instantiate_config_port;
  video_role.pf_port[0] = instantiate_video_port;
  video_role.nports = 1;
  video_role.pf_proc = instantiate_processor;

  strcpy ((OMX_STRING) image_role.role,
          ARATELIA_FILE_READER_IMAGE_READER_ROLE);
  image_role.pf_cport = instantiate_config_port;
  image_role.pf_port[0] = instantiate_image_port;
  image_role.nports = 1;
  image_role.pf_proc = instantiate_processor;

  strcpy ((OMX_STRING) other_role.role,
          ARATELIA_FILE_READER_OTHER_READER_ROLE);
  other_role.pf_cport = instantiate_config_port;
  other_role.pf_port[0] = instantiate_other_port;
  other_role.nports = 1;
  other_role.pf_proc = instantiate_processor;

  tiz_comp_init (ap_hdl, ARATELIA_FILE_READER_COMPONENT_NAME);

  tiz_comp_register_roles (ap_hdl, rf_list, 4);

  return OMX_ErrorNone;
}
