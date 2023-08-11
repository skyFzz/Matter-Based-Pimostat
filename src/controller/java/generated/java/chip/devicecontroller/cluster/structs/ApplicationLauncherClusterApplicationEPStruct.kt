/*
 *
 *    Copyright (c) 2023 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */
package chip.devicecontroller.cluster.structs

import chip.devicecontroller.cluster.*
import chip.tlv.AnonymousTag
import chip.tlv.ContextSpecificTag
import chip.tlv.Tag
import chip.tlv.TlvParsingException
import chip.tlv.TlvReader
import chip.tlv.TlvWriter

import java.util.Optional

class ApplicationLauncherClusterApplicationEPStruct (
    val application: ApplicationLauncherClusterApplicationStruct,
    val endpoint: Optional<Int>) {
  override fun toString(): String  = buildString {
    append("ApplicationLauncherClusterApplicationEPStruct {\n")
    append("\tapplication : $application\n")
    append("\tendpoint : $endpoint\n")
    append("}\n")
  }

  fun toTlv(tag: Tag, tlvWriter: TlvWriter) {
    tlvWriter.apply {
      startStructure(tag)
      application.toTlv(ContextSpecificTag(TAG_APPLICATION), this)
      if (endpoint.isPresent) {
      val optendpoint = endpoint.get()
      put(ContextSpecificTag(TAG_ENDPOINT), optendpoint)
    }
      endStructure()
    }
  }

  companion object {
    private const val TAG_APPLICATION = 0
    private const val TAG_ENDPOINT = 1

    fun fromTlv(tag: Tag, tlvReader: TlvReader) : ApplicationLauncherClusterApplicationEPStruct {
      tlvReader.enterStructure(tag)
      val application = ApplicationLauncherClusterApplicationStruct.fromTlv(ContextSpecificTag(TAG_APPLICATION), tlvReader)
      val endpoint = if (tlvReader.isNextTag(ContextSpecificTag(TAG_ENDPOINT))) {
      Optional.of(tlvReader.getInt(ContextSpecificTag(TAG_ENDPOINT)))
    } else {
      Optional.empty()
    }
      
      tlvReader.exitContainer()

      return ApplicationLauncherClusterApplicationEPStruct(application, endpoint)
    }
  }
}
