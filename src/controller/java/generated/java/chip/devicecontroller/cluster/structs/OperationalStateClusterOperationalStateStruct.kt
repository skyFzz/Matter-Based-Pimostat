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

class OperationalStateClusterOperationalStateStruct (
    val operationalStateID: Int,
    val operationalStateLabel: Optional<String>) {
  override fun toString(): String  = buildString {
    append("OperationalStateClusterOperationalStateStruct {\n")
    append("\toperationalStateID : $operationalStateID\n")
    append("\toperationalStateLabel : $operationalStateLabel\n")
    append("}\n")
  }

  fun toTlv(tag: Tag, tlvWriter: TlvWriter) {
    tlvWriter.apply {
      startStructure(tag)
      put(ContextSpecificTag(TAG_OPERATIONAL_STATE_I_D), operationalStateID)
      if (operationalStateLabel.isPresent) {
      val optoperationalStateLabel = operationalStateLabel.get()
      put(ContextSpecificTag(TAG_OPERATIONAL_STATE_LABEL), optoperationalStateLabel)
    }
      endStructure()
    }
  }

  companion object {
    private const val TAG_OPERATIONAL_STATE_I_D = 0
    private const val TAG_OPERATIONAL_STATE_LABEL = 1

    fun fromTlv(tag: Tag, tlvReader: TlvReader) : OperationalStateClusterOperationalStateStruct {
      tlvReader.enterStructure(tag)
      val operationalStateID = tlvReader.getInt(ContextSpecificTag(TAG_OPERATIONAL_STATE_I_D))
      val operationalStateLabel = if (tlvReader.isNextTag(ContextSpecificTag(TAG_OPERATIONAL_STATE_LABEL))) {
      Optional.of(tlvReader.getString(ContextSpecificTag(TAG_OPERATIONAL_STATE_LABEL)))
    } else {
      Optional.empty()
    }
      
      tlvReader.exitContainer()

      return OperationalStateClusterOperationalStateStruct(operationalStateID, operationalStateLabel)
    }
  }
}
