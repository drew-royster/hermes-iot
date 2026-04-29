const els = {
  gatewayUrl: document.querySelector("#gatewayUrl"),
  deviceId: document.querySelector("#deviceId"),
  firmwareVersion: document.querySelector("#firmwareVersion"),
  useMic: document.querySelector("#useMic"),
  connect: document.querySelector("#connect"),
  disconnect: document.querySelector("#disconnect"),
  interrupt: document.querySelector("#interrupt"),
  sendText: document.querySelector("#sendText"),
  debugText: document.querySelector("#debugText"),
  clearLog: document.querySelector("#clearLog"),
  log: document.querySelector("#log"),
  remoteAudio: document.querySelector("#remoteAudio"),
  connectionDot: document.querySelector("#connectionDot"),
  connectionState: document.querySelector("#connectionState"),
  assistantState: document.querySelector("#assistantState"),
  conversation: document.querySelector("#conversation"),
  sessionId: document.querySelector("#sessionId"),
  heardText: document.querySelector("#heardText"),
  assistantText: document.querySelector("#assistantText"),
};

let pc = null;
let controlChannel = null;
let localStream = null;
let audioContext = null;
let authToken = "";
let deviceId = "";

function defaultDeviceId() {
  const saved = localStorage.getItem("hermes-iot-debug-device-id");
  if (saved) return saved;
  const randomId = crypto.randomUUID?.() ?? `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
  return `browser-${randomId.slice(0, 8)}`;
}

els.gatewayUrl.value = window.location.origin;
els.deviceId.value = defaultDeviceId();

function log(message, payload) {
  const time = new Date().toLocaleTimeString();
  const suffix = payload === undefined ? "" : ` ${JSON.stringify(payload, null, 2)}`;
  els.log.textContent += `[${time}] ${message}${suffix}\n`;
  els.log.scrollTop = els.log.scrollHeight;
}

function setConnected(connected) {
  els.connect.disabled = connected;
  els.disconnect.disabled = !connected;
  els.interrupt.disabled = !connected;
  els.sendText.disabled = !connected || controlChannel?.readyState !== "open";
  els.connectionDot.classList.toggle("live", connected);
  els.connectionDot.classList.remove("error");
}

function setConnectionState(state) {
  els.connectionState.textContent = state;
}

function resetTurnText() {
  els.heardText.textContent = "-";
  els.assistantText.textContent = "-";
}

function api(path) {
  return new URL(path, els.gatewayUrl.value.replace(/\/$/, "")).toString();
}

async function postJson(path, body, { authorized = false } = {}) {
  const headers = { "Content-Type": "application/json" };
  if (authorized) headers.Authorization = `Bearer ${authToken}`;
  const response = await fetch(api(path), {
    method: "POST",
    headers,
    body: JSON.stringify(body),
  });
  if (!response.ok) {
    throw new Error(`${response.status} ${response.statusText}: ${await response.text()}`);
  }
  return response.json();
}

async function getJson(path) {
  const response = await fetch(api(path));
  if (!response.ok) {
    throw new Error(`${response.status} ${response.statusText}: ${await response.text()}`);
  }
  return response.json();
}

async function preflight() {
  await getJson("/health");
  if (els.useMic.checked && !window.isSecureContext) {
    throw new Error(
      "microphone capture requires a secure browser origin. Open http://127.0.0.1:8787/debug/webrtc, or uncheck Use microphone for debug-text-only testing.",
    );
  }
  if (els.useMic.checked && !navigator.mediaDevices?.getUserMedia) {
    throw new Error("this browser does not expose getUserMedia on the current origin. Try 127.0.0.1 or disable Use microphone.");
  }
}

async function createSilentAudioStream() {
  audioContext = new AudioContext({ sampleRate: 48000 });
  const oscillator = new OscillatorNode(audioContext, { frequency: 440 });
  const gain = new GainNode(audioContext, { gain: 0 });
  const destination = new MediaStreamAudioDestinationNode(audioContext);
  oscillator.connect(gain).connect(destination);
  oscillator.start();
  return destination.stream;
}

async function createLocalAudioStream() {
  if (!els.useMic.checked) {
    log("using generated silent audio track; send turns with Debug Text");
    return createSilentAudioStream();
  }
  setConnectionState("opening microphone");
  return navigator.mediaDevices.getUserMedia({
    audio: {
      echoCancellation: true,
      noiseSuppression: true,
      autoGainControl: true,
      channelCount: 1,
    },
  });
}

function sendControl(type, payload = {}) {
  if (!controlChannel || controlChannel.readyState !== "open") {
    throw new Error("control channel is not open");
  }
  controlChannel.send(JSON.stringify({ type, payload, timestamp: new Date().toISOString() }));
}

async function connect() {
  deviceId = els.deviceId.value.trim() || defaultDeviceId();
  localStorage.setItem("hermes-iot-debug-device-id", deviceId);
  setConnectionState("checking gateway");
  setConnected(false);
  resetTurnText();
  await preflight();

  const hello = {
    device_id: deviceId,
    firmware_version: els.firmwareVersion.value.trim() || "browser-dev",
    transport: "webrtc",
    sample_rate_hz: 48000,
    codec: "opus",
    capabilities: ["browser", els.useMic.checked ? "mic" : "silent_audio", "speaker", "debug_text"],
    metadata: {
      user_agent: navigator.userAgent,
      debug_frontend: true,
    },
  };

  const claim = await postJson("/v1/pair/claim", {
    device_id: hello.device_id,
    firmware_version: hello.firmware_version,
    capabilities: hello.capabilities,
  });
  authToken = claim.auth_token;
  els.conversation.textContent = claim.conversation;
  log("claimed", { device_id: claim.device_id, conversation: claim.conversation });

  localStream = await createLocalAudioStream();

  pc = new RTCPeerConnection({
    iceServers: [{ urls: "stun:stun.l.google.com:19302" }],
  });
  controlChannel = pc.createDataChannel("hermes-control");

  pc.ontrack = (event) => {
    els.remoteAudio.srcObject = event.streams[0];
    void els.remoteAudio.play().catch((error) => log("audio autoplay blocked", { message: error.message }));
    log("remote audio track", { kind: event.track.kind });
  };

  pc.onconnectionstatechange = () => {
    setConnectionState(pc.connectionState);
    if (["connected", "connecting"].includes(pc.connectionState)) {
      setConnected(true);
    }
    if (["failed", "closed", "disconnected"].includes(pc.connectionState)) {
      setConnected(false);
      if (pc.connectionState === "failed") els.connectionDot.classList.add("error");
    }
  };

  pc.onicecandidate = async (event) => {
    if (!event.candidate) return;
    try {
      await postJson(
        `/v1/devices/${encodeURIComponent(deviceId)}/webrtc/ice`,
        {
          candidate: event.candidate.candidate,
          sdp_mid: event.candidate.sdpMid,
          sdp_mline_index: event.candidate.sdpMLineIndex,
        },
        { authorized: true },
      );
    } catch (error) {
      log("ice candidate rejected", { message: error.message });
    }
  };

  controlChannel.onopen = () => {
    setConnected(true);
    sendControl("hello", hello);
    log("voice session ready; speak naturally");
  };
  controlChannel.onclose = () => {
    setConnected(false);
    log("data channel closed");
  };
  controlChannel.onmessage = (event) => {
    let payload = event.data;
    try {
      payload = JSON.parse(event.data);
    } catch {
      // Keep raw payloads visible.
    }
    if (payload?.type === "assistant.state") {
      els.assistantState.textContent = payload.payload?.state ?? "-";
    }
    if (payload?.type === "audio.input.level") {
      if (payload.payload?.transcript) {
        els.heardText.textContent = payload.payload.transcript;
      }
      if (payload.payload?.final) {
        log(`heard: ${payload.payload.transcript}`);
      }
      return;
    }
    if (payload?.type === "assistant.text.delta") {
      const text = payload.payload?.text ?? "";
      els.assistantText.textContent = `${els.assistantText.textContent === "-" ? "" : els.assistantText.textContent}${text}`;
      log(`assistant: ${text}`);
      return;
    }
    log("gateway event", payload);
  };

  for (const track of localStream.getAudioTracks()) {
    pc.addTrack(track, localStream);
  }

  setConnectionState("offering");
  const offer = await pc.createOffer();
  await pc.setLocalDescription(offer);

  const answer = await postJson(
    `/v1/devices/${encodeURIComponent(deviceId)}/webrtc/offer`,
    { type: "offer", sdp: offer.sdp, hello },
    { authorized: true },
  );
  els.sessionId.textContent = answer.session_id;
  els.conversation.textContent = answer.conversation;
  await pc.setRemoteDescription({ type: answer.type, sdp: answer.sdp });
  log("answer accepted", { session_id: answer.session_id, conversation: answer.conversation });
}

async function disconnect() {
  if (controlChannel) {
    controlChannel.close();
    controlChannel = null;
  }
  if (pc) {
    pc.close();
    pc = null;
  }
  if (localStream) {
    for (const track of localStream.getTracks()) track.stop();
    localStream = null;
  }
  if (audioContext) {
    await audioContext.close();
    audioContext = null;
  }
  els.remoteAudio.srcObject = null;
  setConnected(false);
  setConnectionState("closed");
}

els.connect.addEventListener("click", async () => {
  try {
    await connect();
  } catch (error) {
    els.connectionDot.classList.add("error");
    setConnectionState("error");
    setConnected(false);
    log("connect failed", { message: error.message });
    await disconnect();
  }
});

els.disconnect.addEventListener("click", () => {
  void disconnect();
});

els.interrupt.addEventListener("click", async () => {
  try {
    await postJson(
      `/v1/devices/${encodeURIComponent(deviceId)}/interrupt`,
      { reason: "browser-debug" },
      { authorized: true },
    );
    log("interrupt sent");
  } catch (error) {
    log("interrupt failed", { message: error.message });
  }
});

els.sendText.addEventListener("click", () => {
  try {
    sendControl("debug.user_text", { text: els.debugText.value, hello: { source: "browser-debug" } });
    log("debug text sent", { text: els.debugText.value });
  } catch (error) {
    log("debug text failed", { message: error.message });
  }
});

els.clearLog.addEventListener("click", () => {
  els.log.textContent = "";
});
