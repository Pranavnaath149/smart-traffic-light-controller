# IoT Smart Traffic Light Controller with Hardware Multiplexer Logic 🚦

An adaptive 4-lane traffic management system built on an ESP32 microcontroller, utilizing a 16:1 analog multiplexer (CD74HC4067) for low-latency vehicle density scanning and an embedded asynchronous web dashboard for remote preemption.

Co-authored and documented as an academic research paper: *"Smart Traffic Light Controller Using Multiplexer-Based Logic and Sensor Input"* (SRM Institute of Science and Technology).

---

## 📸 System Architecture & Dashboard

* **Hardware Prototype:** 16-channel analog multiplexer interfacing 8 IR beam sensors with BJT-amplified LED indicator modules.
* **Control UI:** Hosted directly via ESP32 SoftAP (`TrafficLightESP`) serving dynamic real-time traffic updates every second.

---

## ⚙️ Key Technical Features

* **Hardware-Level Sensor Expansion:** Utilizes a 16:1 multiplexer to poll 8 IR sensor channels across 4 lanes through a single ADC pin, drastically cutting microcontroller GPIO overhead and response latency.
* **Dynamic Density-Based Timing:**
  * **Low Density (1 sensor active):** 4-second green interval.
  * **High Density (2 sensors active):** 7-second blinking green priority interval.
* **Round-Robin Fallback:** If no vehicles are detected for more than 5 seconds, the controller transitions to a fair round-robin cycle without skipping empty lanes.
* **Emergency Vehicle Preemption:** Dedicated web triggers force immediate yellow-to-green overrides for specific emergency corridors.
* **Pedestrian & Night Modes:** Includes an all-red vehicle stop interval for pedestrian safety and flashing amber for low-traffic night hours.

---

## 🔌 Hardware Pin Mapping

| Peripheral / Module | ESP32 Pin | Function |
| :--- | :--- | :--- |
| **MUX Select Lines (S0 - S3)** | GPIO 4, 16, 17, 5 | Binary channel addressing |
| **MUX Signal Output** | GPIO 34 (ADC1) | Analog IR threshold reading |
| **Lane 1 LEDs (R, Y, G)** | GPIO 14, 27, 26 | Signal output |
| **Lane 2 LEDs (R, Y, G)** | GPIO 25, 33, 32 | Signal output |
| **Lane 3 LEDs (R, Y, G)** | GPIO 23, 22, 21 | Signal output |
| **Lane 4 LEDs (R, Y, G)** | GPIO 19, 18, 15 | Signal output |

---

## 📊 Research Outcomes & Testing

* **Latency Reduction:** Multiplexer-based sensor scanning yielded switching delays under 1 ms, outperforming pure software-polling routines.
* **Queue Optimization:** Adaptive density timing reduced simulated intersection idle delay by 25–35% compared to static timer cycles.
* **Energy Consumption:** Low-current BJT driver circuitry reduced overall power demand by ~20% compared to typical relay-driven configurations.

---

## 👥 Authors & Academic Affiliation
* **Pranav Nath** (SRM Institute of Science and Technology)
* **Iris Tricia Biju** (SRM Institute of Science and Technology)
* **Shubham Prasad** (SRM Institute of Science and Technology)
