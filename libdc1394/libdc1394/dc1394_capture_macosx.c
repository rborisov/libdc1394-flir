/*
 * 1394-Based Digital Camera Capture Code for the Control Library
 * Written by Chris Urmson <curmson@ri.cmu.edu>
 * Additions by David Moore <dcm@acm.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>

#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <IOKit/firewire/IOFireWireLibIsoch.h>

#include "config.h"
#include "dc1394_internal.h"
#include "dc1394_control.h"
#include "dc1394_utils.h"
#include "dc1394_macosx.h"
#include "dc1394_capture_macosx.h"

/**********************/
/* Internal functions */
/**********************/

/************************************************************
 _dc1394_basic_setup

 Sets up camera features that are capture type independent

 Returns DC1394_SUCCESS on success, DC1394_FAILURE otherwise
*************************************************************/
dc1394error_t 
_dc1394_capture_basic_setup(dc1394camera_t *camera)
{
  DC1394_CAST_CAMERA_TO_MACOSX(craw, camera);
  dc1394error_t err;

  err=dc1394_video_get_mode(camera,&camera->video_mode);
  DC1394_ERR_RTN(err, "Unable to get current video mode");

  err=dc1394_get_image_size_from_video_mode(camera, camera->video_mode, &craw->capture.frame_width, &craw->capture.frame_height);
  DC1394_ERR_RTN(err,"Could not get width/height from format/mode");

  if (dc1394_is_video_mode_scalable(camera->video_mode)==DC1394_TRUE) {
    unsigned int packet_bytes;
    unsigned int packets_per_frame;
    //fprintf(stderr,"Scalable format detected\n");
    err=dc1394_format7_get_byte_per_packet(camera, camera->video_mode, &packet_bytes);
    DC1394_ERR_RTN(err, "Unable to get format 7 bytes per packet for mode %d", camera->video_mode);
    craw->capture.quadlets_per_packet= packet_bytes /4;

    if (craw->capture.quadlets_per_packet<=0) {
      printf("(%s) No format 7 bytes per packet %d \n", __FILE__, camera->video_mode);
      return DC1394_FAILURE;
    }
    // ensure that quadlet aligned buffers are big enough, still expect
    // problems when width*height  != quadlets_per_frame*4
    if (camera->iidc_version >= DC1394_IIDC_VERSION_1_30) { // if version is 1.30
      err=dc1394_format7_get_packet_per_frame(camera, camera->video_mode, &packets_per_frame);
      DC1394_ERR_RTN(err, "Unable to get format 7 packets per frame %d", camera->video_mode);
      craw->capture.quadlets_per_frame=(packets_per_frame*packet_bytes)/4;
    }
    else {
      // For other specs revisions, we use a trick to determine the total bytes.
      // We don't use the total_bytes register in 1.20 as it has been interpreted in
      // different ways by manufacturers. Thanks to Martin Gramatke for pointing this trick out.
      dc1394color_coding_t color_coding;
      float bpp;
      err=dc1394_format7_get_color_coding(camera,camera->video_mode, &color_coding);
      DC1394_ERR_RTN(err, "Unable to get current color coding");

      err=dc1394_get_bytes_per_pixel(color_coding, &bpp);
      DC1394_ERR_RTN(err, "Unable to infer bpp from color coding");
      packets_per_frame = ((int)(craw->capture.frame_width * craw->capture.frame_height * bpp) +
			   packet_bytes -1) / packet_bytes;
      craw->capture.quadlets_per_frame=(packets_per_frame*packet_bytes)/4;
    }
    
  }
  else {
    err=dc1394_video_get_framerate(camera,&camera->framerate);
    DC1394_ERR_RTN(err, "Unable to get current video framerate");
    
    err=_dc1394_get_quadlets_per_packet(camera->video_mode, camera->framerate, &craw->capture.quadlets_per_packet);
    DC1394_ERR_RTN(err, "Unable to get quadlets per packet");
    
    err= _dc1394_quadlets_from_format(camera, camera->video_mode, &craw->capture.quadlets_per_frame);
    DC1394_ERR_RTN(err,"Could not get quadlets per frame");
  }

  if ((craw->capture.quadlets_per_frame<=0 )||
      (craw->capture.quadlets_per_packet<=0)) {
    return DC1394_FAILURE;
  }
  
  return err;
}

static IOReturn
allocate_port (IOFireWireLibIsochPortRef rem_port,
        IOFWSpeed speed, UInt32 chan)
{
  dc1394camera_t * camera = (*rem_port)->GetRefCon (rem_port);
  //printf ("Allocate channel %lu %p\n", chan, camera);
  camera->iso_channel_is_set = 1;
  camera->iso_channel = chan;
  dc1394_video_set_iso_channel(camera, camera->iso_channel);
  return kIOReturnSuccess;
}

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define DATA_SIZE 12

static void
callback (buffer_info * buffer, NuDCLRef dcl)
{
  dc1394camera_macosx_t * craw;
  dc1394capture_t * capture;
  UInt32 bus, cycle, dma_time, dma_sec, dma_cycle;
  int usec;
  int i;

  if (!buffer) {
    fprintf (stderr, "Error: callback buffer is null\n");
    return;
  }

  craw = (dc1394camera_macosx_t *) buffer->camera;
  capture = &(craw->capture);

  if (buffer->status != BUFFER_EMPTY)
    fprintf (stderr, "Error: buffer %d should have been empty\n",
        buffer->i);

  for (i = 0; i < buffer->num_dcls; i += 30) {
    (*capture->loc_port)->Notify (capture->loc_port,
                                  kFWNuDCLUpdateNotification,
                                  (void **) buffer->dcl_list + i,
                                  MIN (buffer->num_dcls - i, 30));
  }

  buffer->status = BUFFER_FILLED;

  (*craw->iface)->GetBusCycleTime (craw->iface, &bus, &cycle);
  gettimeofday (&buffer->filltime, NULL);

  /* Get the bus timestamp of when the packet was received */
  //dma_time = *(UInt32 *)(capture->databuf.address + buffer->i *
  //    DATA_SIZE + 4);
  //printf ("status %08lx\n", dma_time);
  dma_time = *(UInt32 *)(capture->databuf.address + buffer->i *
      DATA_SIZE + 8);
  //printf ("%08lx\n", dma_time);
  dma_sec = (dma_time & 0xe000000) >> 25;
  dma_cycle = (dma_time & 0x1fff000) >> 12;

  /* Compute how many usec ago the packet was received by comparing
   * the current bus time to the timestamp of the first ISO packet */
  usec = ((((cycle & 0xe000000) >> 25) + 8 - dma_sec) % 8) * 1000000 +
    ((((cycle & 0x1fff000) >> 12) + 8000 - dma_cycle) % 8000) * 125 +
    (cycle & 0xfff) * 125 / 3072;
  //printf ("now %u.%06u\n", buffer->filltime.tv_sec, buffer->filltime.tv_usec);
  //printf ("dma_sec %lx dma_cycle %lx usec %d cycle %lx\n", dma_sec, dma_cycle, usec, cycle);

  /* Subtract usec from the current clock time */
  usec = buffer->filltime.tv_usec - usec;
  while (usec < 0) {
    buffer->filltime.tv_sec--;
    usec += 1000000;
  }
  buffer->filltime.tv_usec = usec;

  //printf ("then %u.%06u\n", buffer->filltime.tv_sec, buffer->filltime.tv_usec);

  if (capture->callback) {
    capture->callback (buffer->camera, capture->callback_user_data);
  }
}

DCLCommand *
CreateDCLProgram (dc1394camera_t * camera)
{
  DC1394_CAST_CAMERA_TO_MACOSX(craw, camera);
  dc1394capture_t * capture = &(craw->capture);
  IOVirtualRange * databuf = &(capture->databuf);
  NuDCLRef dcl = NULL;
  IOFireWireLibNuDCLPoolRef dcl_pool = capture->dcl_pool;
  int packet_size = capture->quadlets_per_packet * 4;
  int bytesperframe = capture->quadlets_per_frame * 4;
  int i;

  databuf->length = (capture->num_frames *
    capture->frame_pages + 1) * getpagesize ();
  databuf->address = (UInt32) mmap (NULL, databuf->length,
      PROT_READ | PROT_WRITE, MAP_ANON, -1, 0);
  if (!databuf->address) {
    fprintf (stderr, "Error: mmap failed\n");
    return NULL;
  }

  for (i = 0; i < capture->num_frames; i++) {
    UInt32 frame_address = databuf->address + (i * capture->frame_pages + 1) *
      getpagesize();
    UInt32 data_address = databuf->address + i * DATA_SIZE;
    int num_dcls = (bytesperframe - 1) / packet_size + 1;
    buffer_info * buffer = capture->buffers + i;
    int j;
    IOVirtualRange ranges[2] = {
      {data_address, 4},
      {frame_address, packet_size},
    };

    dcl = (*dcl_pool)->AllocateReceivePacket (dcl_pool, NULL,
        4, 2, ranges);
    (*dcl_pool)->SetDCLWaitControl (dcl, true);
    (*dcl_pool)->SetDCLFlags (dcl, kNuDCLDynamic);
    (*dcl_pool)->SetDCLStatusPtr (dcl, (UInt32 *)(data_address + 4));
    (*dcl_pool)->SetDCLTimeStampPtr (dcl, (UInt32 *)(data_address + 8));

    buffer->camera = camera;
    buffer->i = i;
    buffer->status = BUFFER_EMPTY;
    buffer->num_dcls = num_dcls;
    buffer->dcl_list = malloc (num_dcls * sizeof (NuDCLRef));

    buffer->dcl_list[0] = dcl;

    for (j = 1; j < num_dcls; j++) {
      ranges[1].address += packet_size;
      dcl = (*dcl_pool)->AllocateReceivePacket (dcl_pool, NULL,
          0, 1, ranges+1);
      buffer->dcl_list[j] = dcl;
    }

    (*dcl_pool)->SetDCLRefcon (dcl, capture->buffers + i);
    (*dcl_pool)->SetDCLCallback (dcl, (NuDCLCallback) callback);
  }
  (*dcl_pool)->SetDCLBranch (dcl, capture->buffers[0].dcl_list[0]);

  dcl = capture->buffers[capture->num_frames-1].dcl_list[0];
  (*dcl_pool)->SetDCLBranch (dcl, dcl);

  //(*dcl_pool)->PrintProgram (dcl_pool);
  return (*dcl_pool)->GetProgram (dcl_pool);
}

/*************************************************************
 CAPTURE SETUP
**************************************************************/
dc1394error_t 
dc1394_capture_setup_dma(dc1394camera_t *camera, uint_t num_dma_buffers,
        dc1394ring_buffer_policy_t policy)
{
  DC1394_CAST_CAMERA_TO_MACOSX(craw, camera);
  dc1394capture_t * capture = &(craw->capture);
  dc1394error_t err;
  IOFireWireLibDeviceRef d = craw->iface;
  IOFWSpeed speed;
  IOFireWireLibIsochChannelRef chan;
  IOFireWireLibRemoteIsochPortRef rem_port;
  IOFireWireLibLocalIsochPortRef loc_port;
  IOFireWireLibNuDCLPoolRef dcl_pool;
  DCLCommand * dcl_program;
  int frame_size;
  int numdcls;

  err = _dc1394_capture_basic_setup(camera);
  DC1394_ERR_RTN (err,"Could not setup capture");

  capture->num_frames = num_dma_buffers;
  capture->ring_buffer_policy = policy;
  capture->chan = NULL;
  capture->rem_port = NULL;
  capture->loc_port = NULL;
  capture->dcl_pool = NULL;
  capture->databuf.address = (vm_address_t) NULL;

  if (!capture->run_loop) {
    dc1394_capture_schedule_with_runloop (camera,
        CFRunLoopGetCurrent (), CFSTR ("dc1394mode"));
  }

  (*d)->AddCallbackDispatcherToRunLoopForMode (d,
                                               capture->run_loop, capture->run_loop_mode);
  (*d)->AddIsochCallbackDispatcherToRunLoopForMode (d,
                                                    capture->run_loop, capture->run_loop_mode);
  (*d)->TurnOnNotification (d);

  (*d)->GetSpeedToNode (d, craw->generation, &speed);
  chan = (*d)->CreateIsochChannel (d, true,
      craw->capture.quadlets_per_packet * 4, kFWSpeed400MBit,
      CFUUIDGetUUIDBytes (kIOFireWireIsochChannelInterfaceID));
  if (!chan) {
    fprintf (stderr, "Could not create IsochChannelInterface\n");
    return DC1394_FAILURE;
  }
  capture->chan = chan;

  rem_port = (*d)->CreateRemoteIsochPort (d, true,
      CFUUIDGetUUIDBytes (kIOFireWireRemoteIsochPortInterfaceID));
  if (!rem_port) {
    fprintf (stderr, "Could not create RemoteIsochPortInterface\n");
    return DC1394_FAILURE;
  }
  capture->rem_port = rem_port;
  (*rem_port)->SetAllocatePortHandler (rem_port, &allocate_port);
  (*rem_port)->SetRefCon ((IOFireWireLibIsochPortRef)rem_port, camera);

  capture->buffers = malloc (capture->num_frames * sizeof (buffer_info));
  capture->current = -1;
  frame_size = capture->quadlets_per_frame * 4;
  capture->frame_pages = ((frame_size - 1) / getpagesize()) + 1;

  numdcls = (capture->quadlets_per_frame / capture->quadlets_per_packet + 1)
    * capture->num_frames;

  dcl_pool = (*d)->CreateNuDCLPool (d, numdcls,
      CFUUIDGetUUIDBytes (kIOFireWireNuDCLPoolInterfaceID));
  if (!dcl_pool) {
    fprintf (stderr, "Could not create NuDCLPoolInterface\n");
    return DC1394_FAILURE;
  }
  capture->dcl_pool = dcl_pool;

  dcl_program = CreateDCLProgram (camera);
  if (!dcl_program) {
    fprintf (stderr, "Could not create DCL Program\n");
    return DC1394_FAILURE;
  }

  loc_port = (*d)->CreateLocalIsochPort (d, false, dcl_program,
      kFWDCLSyBitsEvent, 1, 1, nil, 0, &(capture->databuf), 1,
      CFUUIDGetUUIDBytes (kIOFireWireLocalIsochPortInterfaceID));
  if (!loc_port) {
    fprintf (stderr, "Could not create LocalIsochPortInterface\n");
    return DC1394_FAILURE;
  }
  capture->loc_port = loc_port;

  (*chan)->AddListener (chan, (IOFireWireLibIsochPortRef) loc_port);
  (*chan)->SetTalker (chan, (IOFireWireLibIsochPortRef) rem_port);

  if ((*chan)->AllocateChannel (chan) != kIOReturnSuccess) {
    fprintf (stderr, "Could not allocate channel\n");
    return DC1394_FAILURE;
  }
  if ((*chan)->Start (chan) != kIOReturnSuccess) {
    fprintf (stderr, "Could not start channel\n");
    return DC1394_FAILURE;
  }

  camera->capture_is_set=1;

  return DC1394_SUCCESS;
}

dc1394error_t
dc1394_capture_setup(dc1394camera_t *camera) 
{
  return dc1394_capture_setup_dma (camera, 10, 0);
}

/*****************************************************
 CAPTURE_STOP
*****************************************************/

dc1394error_t 
dc1394_capture_stop(dc1394camera_t *camera) 
{
  DC1394_CAST_CAMERA_TO_MACOSX(craw, camera);
  dc1394capture_t * capture = &(craw->capture);
  IOVirtualRange * databuf = &(capture->databuf);

  if (capture->chan) {
    (*capture->chan)->Stop (capture->chan);
    (*capture->chan)->ReleaseChannel (capture->chan);
  }

  if (databuf->address)
    munmap ((void *) databuf->address, databuf->length);

  if (capture->chan)
    (*capture->chan)->Release (capture->chan);
  if (capture->loc_port)
    (*capture->loc_port)->Release (capture->loc_port);
  if (capture->rem_port)
    (*capture->rem_port)->Release (capture->rem_port);
  if (capture->dcl_pool)
    (*capture->dcl_pool)->Release (capture->dcl_pool);

  if (capture->buffers) {
    int i;
    for (i = 0; i < capture->num_frames; i++)
      free (capture->buffers[i].dcl_list);
    free (capture->buffers);
  }

  capture->chan = NULL;
  capture->rem_port = NULL;
  capture->loc_port = NULL;
  capture->dcl_pool = NULL;
  capture->databuf.address = (vm_address_t) NULL;
  capture->buffers = NULL;

  camera->capture_is_set=0;
  return DC1394_SUCCESS;
}

/****************************************************
 CAPTURE
*****************************************************/
#define NEXT_BUFFER(c,i) (((i) == -1) ? 0 : ((i)+1)%(c)->num_frames)
#define PREV_BUFFER(c,i) (((i) == 0) ? (c)->num_frames-1 : ((i)-1))

dc1394error_t
dc1394_capture_dma(dc1394camera_t **cameras, uint_t num,
    dc1394video_policy_t policy) 
{
  int i;

  for (i = 0; i < num; i++) {
    DC1394_CAST_CAMERA_TO_MACOSX(craw, cameras[i]);
    dc1394capture_t * capture = &(craw->capture);
    int next = NEXT_BUFFER (capture, capture->current);
    buffer_info * buffer = capture->buffers + next;

    while (buffer->status != BUFFER_FILLED) {
      SInt32 code;
      dc1394capture_callback_t callback;
      if (capture->run_loop != CFRunLoopGetCurrent ()) {
        fprintf (stderr, "Error: capturing from wrong thread\n");
        return DC1394_NO_FRAME;
      }

      callback = capture->callback;
      capture->callback = NULL;
      if (policy == DC1394_VIDEO1394_POLL)
        code = CFRunLoopRunInMode (capture->run_loop_mode, 0, true);
      else
        code = CFRunLoopRunInMode (capture->run_loop_mode, 5, true);

      capture->callback = callback;
      //printf ("capture runloop: %ld\n", code);
      if (policy == DC1394_VIDEO1394_POLL &&
          code != kCFRunLoopRunHandledSource)
        return DC1394_NO_FRAME;
    }
  }

  for (i = 0; i < num; i++) {
    DC1394_CAST_CAMERA_TO_MACOSX(craw, cameras[i]);
    dc1394capture_t * capture = &(craw->capture);
    int next = NEXT_BUFFER (capture, capture->current);

    capture->current = next;

    /* Catch up to the latest buffer if the application desires it */
    if (capture->ring_buffer_policy == DC1394_RING_BUFFER_LAST) {
      SInt32 code;
      buffer_info * buffer;
      next = NEXT_BUFFER (capture, capture->current);
      buffer = capture->buffers + next;

      do {
        while (buffer->status == BUFFER_FILLED) {
          capture->current = next;
          next = NEXT_BUFFER (capture, capture->current);
          buffer = capture->buffers + next;
        }
        /* Run the first callback that is pending immediately */
        code = CFRunLoopRunInMode (capture->run_loop_mode, 0, true);
      } while (code == kCFRunLoopRunHandledSource);
    }
  }

  return DC1394_SUCCESS;
}

dc1394error_t 
dc1394_capture(dc1394camera_t **cams, uint_t num) 
{
  int i;
  for (i = 0; i < num; i++) {
    if (dc1394_capture_get_dma_buffer (cams[i]))
      dc1394_capture_dma_done_with_buffer (cams[i]);
  }
  return dc1394_capture_dma (cams, num, DC1394_VIDEO1394_WAIT);
}

/****************************************************
 dc1394_dma_done_with_buffer

 This allows the driver to use the buffer previously
 handed to the user by dc1394_dma_*_capture
*****************************************************/
dc1394error_t 
dc1394_capture_dma_done_with_buffer(dc1394camera_t *camera) 
{  
  DC1394_CAST_CAMERA_TO_MACOSX(craw, camera);
  dc1394capture_t * capture = &(craw->capture);
  int prev = PREV_BUFFER (capture, capture->current);
  buffer_info * buffer = capture->buffers + capture->current;
  buffer_info * prev_buffer = capture->buffers + prev;
  IOFireWireLibNuDCLPoolRef dcl_pool = capture->dcl_pool;
  IOFireWireLibLocalIsochPortRef loc_port = capture->loc_port;
  void * dcl_list[2];

  if (capture->current == -1 || buffer->status != BUFFER_FILLED)
    return DC1394_FAILURE;

  buffer->status = BUFFER_EMPTY;
  (*dcl_pool)->SetDCLBranch (buffer->dcl_list[0], buffer->dcl_list[0]);
  (*dcl_pool)->SetDCLBranch (prev_buffer->dcl_list[0], prev_buffer->dcl_list[1]);
  dcl_list[0] = prev_buffer->dcl_list[0];
  dcl_list[1] = buffer->dcl_list[0];
  (*loc_port)->Notify (loc_port, kFWNuDCLModifyJumpNotification, dcl_list, 1);
  (*loc_port)->Notify (loc_port, kFWNuDCLModifyJumpNotification, dcl_list+1, 1);
  return DC1394_SUCCESS;
}

/* functions to access the capture data */

uchar_t*
dc1394_capture_get_dma_buffer(dc1394camera_t *camera)
{
  DC1394_CAST_CAMERA_TO_MACOSX(craw, camera);
  dc1394capture_t * capture = &(craw->capture);
  buffer_info * buffer = capture->buffers + capture->current;
  IOVirtualRange * databuf = &(capture->databuf);

  if (capture->current == -1 || buffer->status != BUFFER_FILLED)
    return NULL;

  return (uchar_t *) (databuf->address +
      buffer->i * capture->frame_pages * getpagesize() +
      getpagesize());
}

struct timeval*
dc1394_capture_get_dma_filltime(dc1394camera_t *camera)
{
  DC1394_CAST_CAMERA_TO_MACOSX(craw, camera);
  dc1394capture_t * capture = &(craw->capture);
  buffer_info * buffer = capture->buffers + capture->current;

  return &buffer->filltime;
}

int
dc1394_capture_schedule_with_runloop (dc1394camera_t * camera,
        CFRunLoopRef run_loop, CFStringRef run_loop_mode)
{
  DC1394_CAST_CAMERA_TO_MACOSX(craw, camera);
  dc1394capture_t * capture = &(craw->capture);

  if (camera->capture_is_set) {
    fprintf (stderr, "Warning: schedule_with_runloop must be called before setup_dma\n");
    return -1;
  }

  capture->run_loop = run_loop;
  capture->run_loop_mode = run_loop_mode;
  return 0;
}

void
dc1394_capture_set_callback (dc1394camera_t * camera,
        dc1394capture_callback_t callback, void * user_data)
{
  DC1394_CAST_CAMERA_TO_MACOSX(craw, camera);
  dc1394capture_t * capture = &(craw->capture);

  capture->callback = callback;
  capture->callback_user_data = user_data;
}

uint_t
dc1394_capture_get_width(dc1394camera_t *camera)
{
  DC1394_CAST_CAMERA_TO_MACOSX(craw, camera);
  return craw->capture.frame_width;
}

uint_t
dc1394_capture_get_height(dc1394camera_t *camera)
{
  DC1394_CAST_CAMERA_TO_MACOSX(craw, camera);
  return craw->capture.frame_height;
}

uint_t
dc1394_capture_get_bytes_per_frame(dc1394camera_t *camera)
{
  DC1394_CAST_CAMERA_TO_MACOSX(craw, camera);
  return craw->capture.quadlets_per_frame*4;
}

uint_t
dc1394_capture_get_frames_behind(dc1394camera_t *camera)
{
  //DC1394_CAST_CAMERA_TO_MACOSX(craw, camera);
  return 0;
}
