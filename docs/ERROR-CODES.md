# Active Live Delay error-code reference

The dock and OBS log prefix operational failures with a stable code such as
`[ALD-E3002]`. For a manual test report, record the complete visible message,
the code, the OBS log, and the action being performed. Codes identify the
failure class; the text after the code carries the safe, specific detail.

Never include a stream key, password, or the full RTMP publish URL in a report.

| Code range | Meaning | Examples |
|:---|:---|:---|
| `ALD-E1001`–`ALD-E1011` | Direct-output setup | unsupported Output mode, missing service, unavailable H.264/AAC encoder, output creation/start failure |
| `ALD-E2001`–`ALD-E2016` | OBS capture and encoded-media pipeline | capture cannot start, encoder/codec-header failure, controller or FLV mux failure; mode, consumer, multistream configuration/startup, and preflight failures |
| `ALD-E3001`–`ALD-E3007` | RTMP transport | invalid service target, connection/write failure, sender startup, queue saturation, reconnect exhaustion, or isolated secondary failure |
| `ALD-E4001`–`ALD-E4002` | Output health watchdog | output stopped unexpectedly, no encoded video frames within the startup window |
| `ALD-E5001`–`ALD-E5005` | Scene handling | invalid/recursive P4 probe input, invalid/interrupted Holding Scene, or unavailable saved Program Scene |
| `ALD-E6001`–`ALD-E6005` | Dock and operator controls | dock/hotkey registration, conflicting broadcast or delay actions, or destination-settings save failure |

The code meanings are intentionally stable once released. New failures should
receive a new code rather than repurposing an existing one.

## Reference

| Code | Failure class | Safe action / interpretation |
|:---|:---|:---|
| `ALD-E1001` | Direct profile unavailable | Check OBS's active profile and Simple Output configuration. |
| `ALD-E1002` | Direct Output mode unsupported | Use the supported Simple Output direct-start workflow. |
| `ALD-E1003` | Streaming service missing | Configure an OBS streaming service before starting delayed output. |
| `ALD-E1004` | H.264 video encoder unavailable | Select or configure a supported H.264 streaming encoder. |
| `ALD-E1005` | Streaming audio codec unsupported | Configure AAC streaming audio. |
| `ALD-E1006` | AAC audio encoder unavailable | Check the OBS audio encoder configuration. |
| `ALD-E1007` | Encoder settings unavailable | Reopen or repair the active OBS output profile. |
| `ALD-E1008` | Direct encoder creation failed | Check the OBS log and encoder availability. |
| `ALD-E1009` | Direct output creation failed | Check the OBS log and plugin installation. |
| `ALD-E1010` | Streaming service attach failed | Recheck the configured service; do not report credentials. |
| `ALD-E1011` | Direct output start failed | Check the safe dock message and OBS log. |
| `ALD-E2001` | Capture cannot begin | OBS has not made encoded capture ready. |
| `ALD-E2002` | Encoder initialization failed | Check the assigned H.264/AAC encoders. |
| `ALD-E2003` | Output service missing | The plugin output has no assigned OBS service. |
| `ALD-E2004` | Codec headers unavailable | The H.264/AAC encoder did not provide usable configuration. |
| `ALD-E2005` | Capture start failed | OBS refused encoded data capture. |
| `ALD-E2006` | Encoder stopped | Encoded packets ended unexpectedly. |
| `ALD-E2007` | Delay controller failed | The controller rejected media or reached an error state. |
| `ALD-E2008` | FLV mux failed | The encoded media or its timestamps are invalid for FLV. |
| `ALD-E2009` | Generic media-pipeline exception | Unexpected pipeline error; record safe text and OBS log. |
| `ALD-E2010` | Operating-mode conflict | Stop delayed output before changing mode or starting a different mode. |
| `ALD-E2011` | Network-consumer startup failed | FLV/RTMP consumer could not initialize. |
| `ALD-E2012` | Packet consumer failed | A released-packet consumer rejected a media batch. |
| `ALD-E2013` | Discontinuity notification failed | A consumer could not process an epoch/discontinuity notification. |
| `ALD-E2014` | Native Multistream configuration invalid | Stop the output, then check the saved secondary name, RTMP server, and key without sharing them. |
| `ALD-E2015` | Native Multistream primary startup failed | Check the primary service and target configuration; do not report credentials. |
| `ALD-E2016` | Native Multistream preflight failed | The shared rendition exceeds a known hard limit for an enabled platform. Correct the listed OBS output setting before start. |
| `ALD-E3001` | RTMP target invalid | Check the service configuration without exposing the publish URL or key. |
| `ALD-E3002` | RTMP connection failed | Check network and service reachability; keep credentials private. |
| `ALD-E3003` | RTMP write failed | The connected server stopped accepting FLV data. |
| `ALD-E3004` | Sender startup failed | The RTMP sender could not start or complete its initial connection. |
| `ALD-E3005` | Sender queue full | Network delivery cannot keep up with the bounded queue. |
| `ALD-E3006` | Reconnect exhausted | RTMP reconnection attempts were exhausted. |
| `ALD-E3007` | Secondary target failed | The primary can keep streaming. Check the named secondary's reachability and private configuration. |
| `ALD-E4001` | Output stopped unexpectedly | Plugin-owned output stopped outside the requested workflow. |
| `ALD-E4002` | Startup received no video frames | Check capture/encoder progress in the OBS log. |
| `ALD-E5001` | Scene-capture probe input invalid | Select an existing scene as Delay Input. The P4 probe accepts scenes only. |
| `ALD-E5002` | Scene-capture probe recursion blocked | Keep the selected Delay Input separate from every scene containing the probe. |
| `ALD-E5003` | Holding Scene invalid | Select an existing Holding Scene that differs from the current Program Scene. |
| `ALD-E5004` | Holding Scene interrupted | Keep the selected Holding Scene active while delay builds, then retry Start Delay. |
| `ALD-E5005` | Saved Program Scene unavailable | Restore or select the intended Program Scene, then retry Start Delay. |
| `ALD-E6001` | Dock registration failed | Reopen OBS and check the plugin installation and OBS log. |
| `ALD-E6002` | Broadcast control conflict | Stop normal OBS streaming or the already-active plugin broadcast before retrying the requested start action. |
| `ALD-E6003` | Delay control unavailable | Start the plugin broadcast first, or finish/remove the current delay before retrying the delay action. |
| `ALD-E6004` | Destination settings save failed | Check that the active OBS profile is writable before starting the broadcast. |
| `ALD-E6005` | Hotkey registration failed | Use the dock controls; check OBS hotkey configuration and the OBS log before assigning the shortcuts again. |
