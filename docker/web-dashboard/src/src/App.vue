<template>
  <div class="dashboard">
    <h1>GHRA Motor Control</h1>

    <div class="status-bar" :class="{ connected: rosConnected, disconnected: !rosConnected }">
      {{ rosConnected ? 'Connected to ROS2' : 'Disconnected' }}
    </div>

    <div class="panels">
      <!-- Motor Control Panel -->
      <div class="panel">
        <h2>Motor Control</h2>

        <div class="control-row">
          <label>Enable Motor</label>
          <button
            class="toggle-btn"
            :class="{ active: motorEnabled }"
            @click="toggleEnable"
          >
            {{ motorEnabled ? 'ON' : 'OFF' }}
          </button>
        </div>

        <div class="control-row">
          <label>Direction</label>
          <div class="direction-btns">
            <button
              :class="{ active: direction === -1 }"
              @click="setDirection(-1)"
            >REV</button>
            <button
              :class="{ active: direction === 0 }"
              @click="setDirection(0)"
            >STOP</button>
            <button
              :class="{ active: direction === 1 }"
              @click="setDirection(1)"
            >FWD</button>
          </div>
        </div>

        <div class="control-row">
          <label>Speed: {{ Math.round(speedCmd * 100) }}%</label>
          <input
            type="range"
            min="0"
            max="1"
            step="0.01"
            v-model.number="speedCmd"
            @input="publishSpeed"
          />
        </div>

        <div class="readback">
          <span>Speed Feedback: {{ Math.round(speedFeedback * 100) }}%</span>
        </div>
      </div>

      <!-- Position Panel -->
      <div class="panel">
        <h2>Carriage Position</h2>

        <div class="position-display">
          <span class="position-value">{{ positionMm.toFixed(0) }}</span>
          <span class="position-unit">mm</span>
        </div>

        <div class="position-bar-container">
          <div class="position-bar" :style="{ width: positionPercent + '%' }"></div>
        </div>
        <div class="position-labels">
          <span>0 m</span>
          <span>{{ (positionMaxMm / 1000).toFixed(1) }} m</span>
        </div>
      </div>
    </div>
  </div>
</template>

<script>
import ROSLIB from 'roslib'

export default {
  name: 'App',
  data() {
    return {
      ros: null,
      rosConnected: false,
      motorEnabled: false,
      direction: 0,
      speedCmd: 0,
      speedFeedback: 0,
      positionMm: 0,
      positionMaxMm: 15000,
      // ROS topics
      speedCmdTopic: null,
      directionTopic: null,
      enableTopic: null,
    }
  },
  computed: {
    positionPercent() {
      return Math.min(100, Math.max(0, (this.positionMm / this.positionMaxMm) * 100))
    },
  },
  mounted() {
    this.connectRos()
  },
  beforeUnmount() {
    if (this.ros) this.ros.close()
  },
  methods: {
    connectRos() {
      // Connect via WSS through nginx reverse proxy (TLS-secured)
      const protocol = window.location.protocol === 'https:' ? 'wss' : 'ws'
      const rosbridgeUrl = `${protocol}://${window.location.host}/rosbridge`

      this.ros = new ROSLIB.Ros({ url: rosbridgeUrl })

      this.ros.on('connection', () => {
        console.log('Connected to rosbridge')
        this.rosConnected = true
        this.setupTopics()
      })

      this.ros.on('error', (error) => {
        console.error('ROS connection error:', error)
        this.rosConnected = false
      })

      this.ros.on('close', () => {
        console.log('ROS connection closed, reconnecting in 3s...')
        this.rosConnected = false
        setTimeout(() => this.connectRos(), 3000)
      })
    },

    setupTopics() {
      // Publishers
      this.speedCmdTopic = new ROSLIB.Topic({
        ros: this.ros,
        name: '/motor/speed_cmd',
        messageType: 'std_msgs/Float32',
      })
      this.speedCmdTopic.advertise()

      this.directionTopic = new ROSLIB.Topic({
        ros: this.ros,
        name: '/motor/direction',
        messageType: 'std_msgs/Int8',
      })
      this.directionTopic.advertise()

      this.enableTopic = new ROSLIB.Topic({
        ros: this.ros,
        name: '/motor/enable',
        messageType: 'std_msgs/Bool',
      })
      this.enableTopic.advertise()

      // Subscribers
      const speedFeedbackTopic = new ROSLIB.Topic({
        ros: this.ros,
        name: '/motor/speed_feedback',
        messageType: 'std_msgs/Float32',
      })
      speedFeedbackTopic.subscribe((msg) => {
        this.speedFeedback = msg.data
      })

      const positionTopic = new ROSLIB.Topic({
        ros: this.ros,
        name: '/carriage/position',
        messageType: 'std_msgs/Float32',
      })
      positionTopic.subscribe((msg) => {
        this.positionMm = msg.data
      })
    },

    toggleEnable() {
      this.motorEnabled = !this.motorEnabled
      if (!this.motorEnabled) {
        this.speedCmd = 0
        this.direction = 0
      }
      // Publish enable separately, then speed+direction
      if (this.enableTopic) {
        this.enableTopic.publish(new ROSLIB.Message({ data: this.motorEnabled }))
      }
      this.publishMotion()
    },

    setDirection(dir) {
      this.direction = dir
      if (dir === 0) {
        this.speedCmd = 0
      } else {
        // Auto-enable motor when a direction is chosen
        this.motorEnabled = true
      }
      this.publishMotion()
    },

    publishSpeed() {
      this.publishMotion()
    },

    publishMotion() {
      // Direction first — relay response is most noticeable to user
      if (this.directionTopic) {
        this.directionTopic.publish(new ROSLIB.Message({ data: this.direction }))
      }
      if (this.speedCmdTopic) {
        this.speedCmdTopic.publish(new ROSLIB.Message({ data: this.speedCmd }))
      }
    },

    publishDirection() {
      this.publishAll()
    },
  },
}
</script>

<style>
* {
  box-sizing: border-box;
  margin: 0;
  padding: 0;
}

body {
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
  background: #1a1a2e;
  color: #e0e0e0;
}

.dashboard {
  max-width: 800px;
  margin: 0 auto;
  padding: 20px;
}

h1 {
  text-align: center;
  margin-bottom: 16px;
  color: #fff;
}

.status-bar {
  text-align: center;
  padding: 8px;
  border-radius: 6px;
  margin-bottom: 20px;
  font-weight: bold;
}

.status-bar.connected {
  background: #1b4332;
  color: #52b788;
}

.status-bar.disconnected {
  background: #461220;
  color: #e5383b;
}

.panels {
  display: flex;
  flex-direction: column;
  gap: 20px;
}

.panel {
  background: #16213e;
  border-radius: 12px;
  padding: 20px;
}

.panel h2 {
  margin-bottom: 16px;
  color: #a8dadc;
}

.control-row {
  margin-bottom: 16px;
}

.control-row label {
  display: block;
  margin-bottom: 8px;
  color: #ccc;
}

.toggle-btn {
  padding: 10px 32px;
  border: 2px solid #555;
  border-radius: 8px;
  background: #2a2a4a;
  color: #e0e0e0;
  font-size: 16px;
  cursor: pointer;
}

.toggle-btn.active {
  background: #1b4332;
  border-color: #52b788;
  color: #52b788;
}

.direction-btns {
  display: flex;
  gap: 8px;
}

.direction-btns button {
  flex: 1;
  padding: 12px;
  border: 2px solid #555;
  border-radius: 8px;
  background: #2a2a4a;
  color: #e0e0e0;
  font-size: 14px;
  cursor: pointer;
}

.direction-btns button.active {
  background: #0a3d62;
  border-color: #3498db;
  color: #3498db;
}

.direction-btns button:disabled {
  opacity: 0.4;
  cursor: not-allowed;
}

input[type="range"] {
  width: 100%;
  height: 8px;
  -webkit-appearance: none;
  background: #333;
  border-radius: 4px;
  outline: none;
}

input[type="range"]::-webkit-slider-thumb {
  -webkit-appearance: none;
  width: 24px;
  height: 24px;
  border-radius: 50%;
  background: #3498db;
  cursor: pointer;
}

.readback {
  color: #888;
  font-size: 14px;
}

.position-display {
  text-align: center;
  margin-bottom: 12px;
}

.position-value {
  font-size: 48px;
  font-weight: bold;
  color: #fff;
}

.position-unit {
  font-size: 20px;
  color: #888;
  margin-left: 4px;
}

.position-bar-container {
  background: #333;
  border-radius: 6px;
  height: 16px;
  overflow: hidden;
}

.position-bar {
  background: linear-gradient(90deg, #3498db, #2ecc71);
  height: 100%;
  border-radius: 6px;
  transition: width 0.2s ease;
}

.position-labels {
  display: flex;
  justify-content: space-between;
  margin-top: 4px;
  color: #888;
  font-size: 12px;
}
</style>
