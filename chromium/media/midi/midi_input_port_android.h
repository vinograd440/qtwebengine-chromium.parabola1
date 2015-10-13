// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_MIDI_MIDI_INPUT_PORT_ANDROID_H_
#define MEDIA_MIDI_MIDI_INPUT_PORT_ANDROID_H_

#include <jni.h>

#include "base/android/scoped_java_ref.h"
#include "base/time/time.h"

namespace media {
namespace midi {

class MidiInputPortAndroid final {
 public:
  class Delegate {
   public:
    virtual ~Delegate() {}
    virtual void OnReceivedData(MidiInputPortAndroid* port,
                                const uint8* data,
                                size_t size,
                                base::TimeTicks time) = 0;
  };
  MidiInputPortAndroid(JNIEnv* env, jobject raw, Delegate* delegate);
  ~MidiInputPortAndroid();

  // Returns true when the operation succeeds.
  bool Open();
  void Close();

  // Called by the Java world.
  void OnData(JNIEnv* env,
              jobject caller,
              jbyteArray data,
              jint offset,
              jint size,
              jlong timestamp);

  static bool Register(JNIEnv* env);

 private:
  base::android::ScopedJavaGlobalRef<jobject> raw_port_;
  Delegate* const delegate_;
};

}  // namespace midi
}  // namespace media

#endif  // MEDIA_MIDI_MIDI_INPUT_PORT_ANDROID_H_
