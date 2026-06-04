#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include "capture.hpp"

@interface PipetteStreamOutput : NSObject <SCStreamOutput>
@property(nonatomic, assign) Pipette::FrameCallback callback;
@end

@implementation PipetteStreamOutput

- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type {

  if (type != SCStreamOutputTypeScreen)
    return;

  // 1. Extract pixel Buffer from Sample Buffer
  CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
  if (!imageBuffer)
    return;

  // 2. Ock so we can read the bytes
  // basically put a mutex on it so it cannot be overwritten
  // or otherwise telling the OS not to move the memory while i am readin it
  CVPixelBufferLockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);

  // 3 get dimensions and pointer to the latter
  uint8_t *data = (uint8_t *)CVPixelBufferGetBaseAddress(imageBuffer);
  std::size_t width = CVPixelBufferGetWidth(imageBuffer);
  std::size_t height = CVPixelBufferGetHeight(imageBuffer);
  std::size_t size = CVPixelBufferGetDataSize(imageBuffer);

  // 4. get timestamp
  CMTime time = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
  uint64_t timestamp_us = (uint64_t)(CMTimeGetSeconds(time) * 1e6);

  // 5. callback into C++
  //
  // NOTE: Do not  unlock the memory bfeore completing the Callback otherwise
  // the OS will reclaim the memory and the data will be lost
  Pipette::Frame frame{data, size, (uint32_t)width, (uint32_t)height,
                       timestamp_us};
  if (self.callback)
    self.callback(frame);

  // 6. unlock frame so we can overwrite and get next frame
  // also prevents locked memory collection or well memory corruption
  CVPixelBufferUnlockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
};

@end

// keep these alive for the lifetime of capture session
static SCStream *gStream = nil;
static PipetteStreamOutput *gOutput = nil;

// WARN: I DID NOT MAKE THE STUFF BELOW; ASK AI..... modern programming
// languages are weird
namespace Pipette {

void Capture::start(FrameCallback cb) {
  // 1. Ask macOS what we can capture
  [SCShareableContent getShareableContentWithCompletionHandler:^(
                          SCShareableContent *content, NSError *error) {
    if (error) {
      NSLog(@"SCShareableContent error: %@", error);
      return;
    }

    // 2. Pick the first display
    SCDisplay *display = content.displays.firstObject;
    if (!display) {
      NSLog(@"No displays found");
      return;
    }

    // 3. Configure the stream — 1080p, 30fps, BGRA pixels.
    // Match the Pi's 1920x1080 framebuffer so frames arrive 1:1 and the Pi
    // doesn't have to upscale (which softened the image). The Mac scales the
    // display down to this size on the GPU, which looks far better than the
    // Pi's nearest-neighbour blit.
    SCStreamConfiguration *config = [[SCStreamConfiguration alloc] init];
    config.width = 1920;
    config.height = 1080;
    config.minimumFrameInterval = CMTimeMake(1, 30); // up to 30 fps
    config.pixelFormat = kCVPixelFormatType_32BGRA;

    // 4. Create a filter — capture the whole display
    SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:display
                                                      excludingWindows:@[]];

    // 5. Create the delegate and hand it the C++ callback
    gOutput = [[PipetteStreamOutput alloc] init];
    gOutput.callback = cb;

    // 6. Create and start the stream
    gStream = [[SCStream alloc] initWithFilter:filter
                                 configuration:config
                                      delegate:nil];

    NSError *addErr = nil;
    [gStream addStreamOutput:gOutput
                        type:SCStreamOutputTypeScreen
          sampleHandlerQueue:dispatch_queue_create("pipette.capture",
                                                   DISPATCH_QUEUE_SERIAL)
                       error:&addErr];

    [gStream startCaptureWithCompletionHandler:^(NSError *e) {
      if (e)
        NSLog(@"startCapture error: %@", e);
      else
        NSLog(@"Capture started");
    }];
  }];
}

void Capture::stop() {
  [gStream stopCaptureWithCompletionHandler:^(NSError *e) {
    if (e)
      NSLog(@"stopCapture error: %@", e);
  }];
  gStream = nil;
  gOutput = nil;
}

} // namespace Pipette
