// snmp_agent.h -- minimal read-only SNMPv1/v2c agent for the ESP32-S3.
//
// Exists so an existing NMS (Cacti, LibreNMS, observium, ad-hoc snmpwalk) can
// poll the board DIRECTLY, without the Raspberry Pi half of the project. The
// Pi-side responder (pi/app/snmp_responder.py, enterprise subtree .99999.7)
// covers the v2 telematics build; this one lives on **.99999.8** so both can
// run on the same network without colliding.
//
// Scope, deliberately small:
//   * SNMPv1 (version 0) and SNMPv2c (version 1)
//   * GET, GETNEXT, GETBULK  -- read-only. SET is refused.
//   * A flat table of scalar OIDs, all BASE.<leaf>.0
//   * No traps, no standard host MIBs (sysUpTime/ifTable/hrStorage). Poll the
//     host those normally come from; this tree is vroom-specific telemetry.
//
// Everything is answered from live globals via getter callbacks, so a poll
// always reflects the same numbers the dashboard is showing.

#pragma once
#include <Arduino.h>
#include <WiFiUdp.h>

// BER / SNMP type tags.
enum SnmpType : uint8_t {
  SNMP_INT       = 0x02,
  SNMP_OCTET     = 0x04,
  SNMP_NULL      = 0x05,
  SNMP_OIDTYPE   = 0x06,
  SNMP_SEQUENCE  = 0x30,
  SNMP_COUNTER32 = 0x41,
  SNMP_GAUGE32   = 0x42,
  SNMP_TIMETICKS = 0x43,
  // v2c exception values, carried in place of a varbind value
  SNMP_NOSUCHOBJECT   = 0x80,
  SNMP_NOSUCHINSTANCE = 0x81,
  SNMP_ENDOFMIBVIEW   = 0x82,
};

// A resolved value. Only the field matching `type` is meaningful.
struct SnmpValue {
  SnmpType type;
  int32_t  i;     // SNMP_INT
  uint32_t u;     // COUNTER32 / GAUGE32 / TIMETICKS
  String   s;     // OCTET STRING
};

typedef void (*SnmpGetter)(SnmpValue& out);

// One scalar OID: BASE.<leaf>.0
struct SnmpEntry {
  uint16_t    leaf;
  SnmpGetter  get;
  const char* name;      // documentation / serial debug only
};

class SnmpAgent {
public:
  // table/count must outlive the agent (a static array in the sketch).
  bool begin(uint16_t port, const char* community,
             const SnmpEntry* table, size_t count);
  void poll();                       // call from loop(); non-blocking
  uint32_t requests() const { return _reqs; }
  uint16_t port() const     { return _port; }

private:
  WiFiUDP          _udp;
  uint16_t         _port = 161;
  String           _community = "public";
  const SnmpEntry* _tab = nullptr;
  size_t           _n = 0;
  uint32_t         _reqs = 0;
  bool             _up = false;

  int  handle(const uint8_t* in, int len, uint8_t* out, int outMax);
};
