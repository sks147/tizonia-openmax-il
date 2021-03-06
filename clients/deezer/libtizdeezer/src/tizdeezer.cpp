/**
 * Copyright (C) 2011-2017 Aratelia Limited - Juan A. Rubio
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
 * @file   tizdeezer.cpp
 * @author Juan A. Rubio <juan.rubio@aratelia.com>
 *
 * @brief  Tizonia - Simple Deezer client library
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <iostream>
#include <boost/lexical_cast.hpp>

#include "tizdeezer.hpp"

namespace bp = boost::python;

/* This macro assumes the existence of an "int rc" local variable */
#define try_catch_wrapper(expr)                                  \
  do                                                             \
    {                                                            \
      try                                                        \
        {                                                        \
          (expr);                                                \
        }                                                        \
      catch (bp::error_already_set & e)                          \
        {                                                        \
          PyErr_PrintEx (0);                                     \
          rc = 1;                                                \
        }                                                        \
      catch (const std::exception &e)                            \
        {                                                        \
          std::cerr << e.what ();                                \
        }                                                        \
      catch (...)                                                \
        {                                                        \
          std::cerr << std::string ("Unknown exception caught"); \
        }                                                        \
    }                                                            \
  while (0)

namespace
{
  void init_deezer (boost::python::object &py_main,
                    boost::python::object &py_global)
  {
    Py_Initialize ();

    // Import the Deezer proxy module
    py_main = bp::import ("tizdeezerproxy");

    // Retrieve the main module's namespace
    py_global = py_main.attr ("__dict__");
  }

  void start_deezer (boost::python::object &py_global,
                     boost::python::object &py_gm_proxy,
                     const std::string &oauth_token)
  {
    bp::object pydeezerproxy = py_global["tizdeezerproxy"];
    py_gm_proxy
        = pydeezerproxy (oauth_token.c_str ());
  }
}

tizdeezer::tizdeezer (const std::string &oauth_token)
  : oauth_token_ (oauth_token)
{
}

tizdeezer::~tizdeezer ()
{
}

int tizdeezer::init ()
{
  int rc = 0;
  try_catch_wrapper (init_deezer (py_main_, py_global_));
  return rc;
}

int tizdeezer::start ()
{
  int rc = 0;
  try_catch_wrapper (
      start_deezer (py_global_, py_gm_proxy_, oauth_token_));
  return rc;
}

void tizdeezer::stop ()
{
  int rc = 0;
  // try_catch_wrapper (py_gm_proxy_.attr ("logout")());
  (void)rc;
}

void tizdeezer::deinit ()
{
  // boost::python doesn't support Py_Finalize() yet!
}

void tizdeezer::set_playback_mode (const playback_mode mode)
{
  int rc = 0;
  switch(mode)
    {
    case PlaybackModeNormal:
      {
        try_catch_wrapper (py_gm_proxy_.attr ("set_play_mode")("NORMAL"));
      }
      break;
    case PlaybackModeShuffle:
      {
        try_catch_wrapper (py_gm_proxy_.attr ("set_play_mode")("SHUFFLE"));
      }
      break;
    default:
      {
        assert (0);
      }
      break;
    };
  (void)rc;
}

int tizdeezer::get_current_track ()
{
  int rc = 1;
  current_user_.clear ();
  current_title_.clear ();

  const bp::tuple &info1 = bp::extract< bp::tuple >(
      py_gm_proxy_.attr ("current_track_title_and_user")());
  const char *p_user = bp::extract< char const * >(info1[0]);
  const char *p_title = bp::extract< char const * >(info1[1]);

  if (p_user)
    {
      current_user_.assign (p_user);
    }
  if (p_title)
    {
      current_title_.assign (p_title);
    }

  int duration
      = bp::extract< int >(py_gm_proxy_.attr ("current_track_duration")());

  int seconds = 0;
  current_duration_.clear ();
  if (duration)
    {
      duration /= 1000;
      seconds = duration % 60;
      int minutes = (duration - seconds) / 60;
      int hours = 0;
      if (minutes >= 60)
        {
          int total_minutes = minutes;
          minutes = total_minutes % 60;
          hours = (total_minutes - minutes) / 60;
        }

      if (hours > 0)
        {
          current_duration_.append (boost::lexical_cast< std::string >(hours));
          current_duration_.append ("h:");
        }

      if (minutes > 0)
        {
          current_duration_.append (
              boost::lexical_cast< std::string >(minutes));
          current_duration_.append ("m:");
        }
    }

  char seconds_str[3];
  sprintf (seconds_str, "%02i", seconds);
  current_duration_.append (seconds_str);
  current_duration_.append ("s");

  const int track_year = bp::extract< int >(py_gm_proxy_.attr ("current_track_year")());
  current_track_year_.assign (boost::lexical_cast< std::string >(track_year));

  const char *p_track_permalink = bp::extract< char const * >(
      py_gm_proxy_.attr ("current_track_permalink")());
  if (p_track_permalink)
    {
      current_track_permalink_.assign (p_track_permalink);
    }

  const char *p_track_license = bp::extract< char const * >(
      py_gm_proxy_.attr ("current_track_license")());
  if (p_track_license)
    {
      current_track_license_.assign (p_track_license);
    }

  const int track_likes = bp::extract< int >(py_gm_proxy_.attr ("current_track_likes")());
  current_track_likes_.assign (boost::lexical_cast< std::string >(track_likes));

  if (p_user || p_title)
    {
      rc = 0;
    }

  return rc;
}
