// scratch/map_data.js
// Master Data Dictionary for FishPump ESP32-S3 Firmware Architecture & Data Flow

const SYSTEM_MAP_DATA = {
  layers: [
    {
      id: "layer_hardware",
      num: 1,
      title: "Layer 1: Physical Hardware & I/O",
      thaiTitle: "ชั้นที่ 1: ฮาร์ดแวร์จริงและสัญญาณอินพุต/เอาต์พุต (Physical I/O)",
      desc: "รีเลย์ขับโหลด, สวิตช์ลูกลอยระดับน้ำ, หัววัดอุณหภูมิดิจิทัล 1-Wire, ปุ่มกด และไฟสถานะ LED",
      color: "#38bdf8"
    },
    {
      id: "layer_tasks",
      num: 2,
      title: "Layer 2: Real-Time Control & FreeRTOS Tasks",
      thaiTitle: "ชั้นที่ 2: งานควบคุมเรียลไทม์และ FreeRTOS Tasks",
      desc: "State Machines ควบคุมปั๊มน้ำ, ระบบทำความเย็นแบบมีฮิสเทอรีซิส, ปุ่มกดกู้คืน และ Task Watchdog",
      color: "#34d399"
    },
    {
      id: "layer_bus_core",
      num: 3,
      title: "Layer 3: System Core, Data Bus & Persistence",
      thaiTitle: "ชั้นที่ 3: แกนระบบ, บัสส่งข้อมูลในแรม และหน่วยความจำถาวร",
      desc: "app_main ผู้ประสานงานหลัก, telemetry_hub บัสกระจายข้อมูล, nvs_store เก็บข้อมูลแฟลช, ความปลอดภัย session",
      color: "#fbbf24"
    },
    {
      id: "layer_network_ui",
      num: 4,
      title: "Layer 4: Connectivity & Presentation",
      thaiTitle: "ชั้นที่ 4: การเชื่อมต่อเครือข่ายและเว็บอินเทอร์เฟซ",
      desc: "Wi-Fi Dual-mode (AP+STA), DNS Captive Portal, HTTP REST APIs, SSE Realtime Stream และ Web SPA",
      color: "#c084fc"
    }
  ],

  components: {
    // --- Layer 1: Hardware ---
    hardware_relays: {
      layer: "layer_hardware",
      id: "hardware_relays",
      thaiName: "รีเลย์ควบคุมกำลังไฟฟ้า (Pump 1, 2 & Chiller)",
      engName: "Power Output Relays",
      sourceFile: "Hardware Circuit / Relay Safe Init",
      rtosContext: { core: "N/A", priority: "Hardware", stack: "N/A", rate: "Immediate" },
      desc: "รีเลย์ตัด-ต่อไฟฟ้าขับปั๊มน้ำหลัก (Pump 1/2) และคอมเพรสเซอร์ทำความเย็น (Cooling) ควบคุมผ่าน GPIO ด้วย Active-Low logic พร้อมระบบ Safe Clamping",
      responsibilities: [
        "ขับมอเตอร์ปั๊มน้ำหมุนเวียนคู่ (GPIO 10, GPIO 11)",
        "ขับคอมเพรสเซอร์ระบายความร้อนตู้ปลา (GPIO 12)",
        "รับสัญญาณลอจิกจาก pump_control และ cooling_control"
      ],
      safetyFeatures: ["Glitch-free Safe Clamping ตอนบูต", "ป้องกันการสลับรีเลย์ถี่ด้วย Minimum Dwell Time"],
      keyAPIs: ["relay_safe_init_pins()", "relay_safe_configure()", "gpio_set_level()"]
    },
    hardware_sensors: {
      layer: "layer_hardware",
      id: "hardware_sensors",
      thaiName: "เซนเซอร์ระดับน้ำและอุณหภูมิ (Float Switch & DS18B20)",
      engName: "Sensors & Level Input",
      sourceFile: "Hardware Circuit / 1-Wire Bus",
      rtosContext: { core: "N/A", priority: "Hardware", stack: "N/A", rate: "Continuous" },
      desc: "สวิตช์ลูกลอยดิจิทัลตรวจจับน้ำแห้ง (Float Switch GPIO 4) และหัววัดอุณหภูมิความแม่นยำสูง DS18B20 สื่อสารผ่านบัส 1-Wire (GPIO 5)",
      responsibilities: [
        "ตรวจจับระดับน้ำในบ่อ/ตู้ปลา (Dry-run protection)",
        "ส่งสัญญาณอุณหภูมิน้ำแบบดิจิทัลความละเอียด 9-12 bit",
        "มีวงจร Pull-up ป้องกันสัญญาณลอย (Floating)"
      ],
      safetyFeatures: ["CRC Check ป้องกันข้อมูลอุณหภูมิเพี้ยน", "ตรวจจับเซนเซอร์หลุดเพื่อตัดไฟอัตโนมัติ"],
      keyAPIs: ["gpio_get_level()", "ds18b20_trigger_temperature_conversion()", "ds18b20_get_temperature()"]
    },
    hardware_ui_buttons: {
      layer: "layer_hardware",
      id: "hardware_ui_buttons",
      thaiName: "ปุ่มกดสั่งงานและไฟสถานะ (Buttons & WS2812 LED)",
      engName: "Physical Controls & Status LED",
      sourceFile: "Hardware Circuit / GPIO 0, 21, 48",
      rtosContext: { core: "N/A", priority: "Hardware", stack: "N/A", rate: "Immediate" },
      desc: "ปุ่มกด Boot บนบอร์ด (GPIO 0), ปุ่มกดภายนอก (GPIO 21) และไฟ LED แสดงสถานะการทำงาน (GPIO 48)",
      responsibilities: [
        "รับการกดสั่งงานฉุกเฉินจากผู้ใช้",
        "แสดงไฟสถานะ: ปกติ, รอยืนยัน Staging, Recovery AP, Factory Reset",
        "มีกลไก Mutual Exclusion ป้องกันการกด 2 ปุ่มพร้อมกัน"
      ],
      safetyFeatures: ["Hardware & Software Debounce", "การป้องกันสวิตช์ค้างด้วย release veto check"],
      keyAPIs: ["gpio_get_level(BOOT_BTN)", "gpio_get_level(EXT_BTN)", "update_status_leds()"]
    },

    // --- Layer 2: Tasks ---
    pump_control: {
      layer: "layer_tasks",
      id: "pump_control",
      thaiName: "ระบบควบคุมปั๊มน้ำ (Pump Control Task)",
      engName: "Pump Control State Machine",
      sourceFile: "firmware/components/pump_control/pump_control.c",
      rtosContext: { core: "Core 0", priority: "5 (High)", stack: "4,096 B", rate: "100 ms" },
      desc: "FreeRTOS Task ควบคุม State Machine ของปั๊มน้ำ สลับการทำงานตามช่วงเวลา Timer 1/2, โหมดล้าง (Wash Cycle), ตรวจสอบสวิตช์ลูกลอย และป้องกันการกระชากของรีเลย์",
      responsibilities: [
        "ประมวลผล FSM: Idle, Water Run, Wash Run, Float Low Wait",
        "สลับการทำงานระหว่างปั๊ม 1 และ 2 ตามที่ตั้งค่าใน NVS",
        "ตรวจจับน้ำแห้งและตัดการทำงานของปั๊มทันทีเพื่อป้องกันปั๊มไหม้",
        "ส่ง telemetry_event_t (PUMP, FLOAT) เข้าสู่ telemetry_hub"
      ],
      safetyFeatures: ["Min Dwell Protection หน่วงเวลาสลับรีเลย์", "ตัดการทำงานทันทีเมื่อลูกลอยตรวจพบน้ำแห้ง"],
      keyAPIs: ["pump_control_start_task()", "pump_control_set_mode()", "pump_control_start_wash()", "telemetry_hub_publish()"]
    },
    cooling_control: {
      layer: "layer_tasks",
      id: "cooling_control",
      thaiName: "ระบบควบคุมอุณหภูมิน้ำ (Cooling Control Task)",
      engName: "Thermostat & Chiller Controller",
      sourceFile: "firmware/components/cooling_control/cooling_control.c",
      rtosContext: { core: "Core 0", priority: "4 (Med-High)", stack: "4,096 B", rate: "1,000 ms (1 Hz)" },
      desc: "FreeRTOS Task ดึงค่าอุณหภูมิจาก DS18B20 ผ่าน 1-Wire, คำนวณตรรกะ Hysteresis สั่งเปิด-ปิดรีเลย์ และป้องกันคอมเพรสเซอร์ด้วย Anti-Short Cycle Delay",
      responsibilities: [
        "อ่านค่าอุณหภูมิ DS18B20 ทุกๆ 1 วินาที พร้อมตรวจสอบ CRC",
        "รัน FSM: Idle, Chilling, Compressor Lockout, Sensor Safe Off",
        "ป้องกันคอมเพรสเซอร์พังด้วยตัวจับเวลา Lockout 180 วินาที",
        "ส่ง telemetry_event_t (COOLING) เข้าสู่ telemetry_hub"
      ],
      safetyFeatures: ["Anti-Short Cycle Min-Off Lockout 180s", "Sensor Fault Safe-Off ตัดรีเลย์ทันทีเมื่อเซนเซอร์มีปัญหา"],
      keyAPIs: ["cooling_control_start_task()", "cooling_control_set_mode()", "apply_cooling_settings_to_config()", "telemetry_hub_publish()"]
    },
    hardware_ui_task: {
      layer: "layer_tasks",
      id: "hardware_ui_task",
      thaiName: "งานตรวจจับปุ่มกดและไฟแสดงผล (Hardware UI Task)",
      engName: "Button & LED Observer",
      sourceFile: "firmware/main/app_main.c (hardware_ui_task)",
      rtosContext: { core: "Core 1", priority: "2 (Low-Med)", stack: "4,096 B", rate: "50 ms" },
      desc: "FreeRTOS Task บน Core 1 ตรวจสอบระยะเวลาการกดปุ่ม Boot/Ext เพื่อเข้าสู่ Recovery AP หรือทำการล้างค่าโรงงาน (Factory Reset) และขับไฟ LED",
      responsibilities: [
        "สแกนปุ่มกดด้วยอัตรา 50ms พร้อม Debounce",
        "กดค้าง 2-5 วินาที: บังคับเปิด Recovery AP (FishPump_Setup)",
        "กดค้าง > 5 วินาที: Trigger สัญญาณ Factory Reset ล้าง NVS ทั้งหมด",
        "อัปเดตรูปแบบไฟกะพริบ LED ตามสถานะระบบ (เช่น Staging Pending)"
      ],
      safetyFeatures: ["Mutual Exclusion ป้องกันการกด 2 ปุ่มพร้อมกัน", "ป้องการ Trigger ซ้ำซ้อนด้วย Atomic Flags"],
      keyAPIs: ["wifi_manager_start_recovery_ap()", "nvs_store_factory_reset()", "update_status_leds()"]
    },
    twdt: {
      layer: "layer_tasks",
      id: "twdt",
      thaiName: "ระบบเฝ้าระวังความปลอดภัยฮาร์ดแวร์ (Task Watchdog 10s)",
      engName: "Task Watchdog Timer (TWDT)",
      sourceFile: "ESP-IDF esp_task_wdt",
      rtosContext: { core: "Hardware / Core 0", priority: "Hardware ISR", stack: "Kernel", rate: "10,000 ms Timeout" },
      desc: "วงจรเฝ้าระวังฮาร์ดแวร์ของ ESP32-S3 ตรวจสอบว่า app_main loop รันปกติและให้อาหาร Watchdog สม่ำเสมอ หาก Task ติดล็อกจะรีเซ็ตชิปทันที",
      responsibilities: [
        "จับเวลา Timeout 10 วินาที",
        "รีเซ็ต CPU ทันที (Panic Reset) หากไม่ได้รับสัญญาณ Keep-Alive",
        "ป้องกันอุปกรณ์ค้างจาก Deadlock หรือ Infinite Loop กลางสนาม"
      ],
      safetyFeatures: ["Hardware-level CPU Reset", "บันทึกสาเหตุการรีเซ็ตลง Flash ก่อนเครื่องดับ"],
      keyAPIs: ["esp_task_wdt_init()", "esp_task_wdt_add()", "esp_task_wdt_reset()"]
    },

    // --- Layer 3: Core & Bus ---
    app_main: {
      layer: "layer_bus_core",
      id: "app_main",
      thaiName: "แกนหลักควบคุมระบบ (System Coordinator / App Main)",
      engName: "System Lifecycle & Coordinator",
      sourceFile: "firmware/main/app_main.c",
      rtosContext: { core: "Core 0", priority: "1 (Main Loop)", stack: "8,192 B", rate: "5,000 ms (5s Loop)" },
      desc: "ศูนย์กลางควบคุมวงจรชีวิต (Lifecycle) ของเฟิร์มแวร์ จัดการลำดับการบูตระบบ, ตรวจสุขภาพ (Health Telemetry), ป้อน TWDT, จัดการ Rollback Timers และตรวจเช็คคิว HTTP",
      responsibilities: [
        "ประสานงานขั้นตอนการบูตเครื่องตามลำดับความปลอดภัยอย่างเคร่งครัด",
        "วนลูปทุก 5 วินาทีเพื่อรีเซ็ต TWDT และส่ง Health Telemetry สู่ Hub",
        "ตรวจจับ HTTP Queue Full ติดต่อกัน 3 ครั้ง (15s) เพื่อสั่งรีบูตป้องกันเว็บค้าง",
        "จัดการตัวจับเวลา Staging Rollback (180s) เพื่อป้องกันการตั้งค่าผิดพลาด"
      ],
      safetyFeatures: ["Emergency Rollback Timer 180s", "HTTP Server Queue Health Monitor", "Crash Detail Logger"],
      keyAPIs: ["app_main()", "esp_task_wdt_reset()", "telemetry_hub_publish()", "nvs_store_rollback_staging()", "esp_restart()"]
    },
    telemetry_hub: {
      layer: "layer_bus_core",
      id: "telemetry_hub",
      thaiName: "บัสกระจายข้อมูลและแคชสถานะ (Telemetry Hub Bus)",
      engName: "In-Memory Pub/Sub Message Broker",
      sourceFile: "firmware/components/telemetry_hub/telemetry_hub.c",
      rtosContext: { core: "Both Cores", priority: "Thread-Safe", stack: "In-Memory", rate: "Event-driven" },
      desc: "ตัวกลางรับ-ส่งข้อมูลสถานะระบบแบบ Decoupled จัดเก็บ Snapshot สถานะล่าสุดใน RAM และกระจาย Event ไปยัง Callback ของผู้ติดตาม (Subscribers) อย่างปลอดภัยด้วย Mutex",
      responsibilities: [
        "รับ Event จาก pump_control, cooling_control และ app_main",
        "ป้องกันข้อมูลชนกันระหว่าง Core ด้วย s_telemetry_mutex",
        "เก็บข้อมูลล่าสุดของระบบ (Pump, Float, Temp, Health) ใน telemetry_snapshot_t",
        "เรียก Callback ของ web_server ทันทีที่มีการเปลี่ยนแปลงสถานะ"
      ],
      safetyFeatures: ["Non-blocking Subscriber Callback Constraint", "FreeRTOS Mutex Reentrancy Protection"],
      keyAPIs: ["telemetry_hub_init()", "telemetry_hub_subscribe()", "telemetry_hub_publish()", "telemetry_hub_get_snapshot()"]
    },
    nvs_store: {
      layer: "layer_bus_core",
      id: "nvs_store",
      thaiName: "คลังข้อมูลถาวรใน Flash (NVS Store & Staging)",
      engName: "Flash Persistence & Staging Engine",
      sourceFile: "firmware/components/nvs_store/nvs_store.c",
      rtosContext: { core: "Both Cores", priority: "Thread-Safe", stack: "Flash Sector", rate: "On-demand" },
      desc: "Single Source of Truth จัดเก็บการตั้งค่าทั้งหมดลงใน Flash Memory แบบ Atomic พร้อมพื้นที่ Staging พิเศษสำหรับทดสอบการตั้งค่าใหม่ก่อนกดยืนยัน",
      responsibilities: [
        "บันทึกและโหลด Wi-Fi Multi-profile (สูงสุด 5 รายการ)",
        "จัดเก็บค่าตัวจับเวลาของปั๊มน้ำ (Pump Settings) และระบบทำความเย็น",
        "จัดเก็บแผนผังขาพินปัจจุบัน (Active Map) และขาพินใหม่ (Pending Map)",
        "บริหารจัดการ Staging Type และดำเนินการ Rollback กู้คืนค่าเดิมอัตโนมัติ"
      ],
      safetyFeatures: ["CRC Data Integrity Verification", "Dual-Slot Staging & Auto-Rollback", "Flash Wear-Leveling"],
      keyAPIs: ["nvs_store_init()", "nvs_store_load_pump_settings()", "nvs_store_save_staging()", "nvs_store_rollback_staging()", "nvs_store_confirm_staging()"]
    },
    hardware_map: {
      layer: "layer_bus_core",
      id: "hardware_map",
      thaiName: "ตัวจัดสรรและตรวจสอบขาพิน (Hardware Map Validator)",
      engName: "GPIO Conflict & Safe Pin Checker",
      sourceFile: "firmware/components/hardware_map/hardware_map.c",
      rtosContext: { core: "Core 0", priority: "Logic Library", stack: "Stack", rate: "On Boot / Config" },
      desc: "โมดูลตรวจสอบความถูกต้องของการกำหนดขาพิน GPIO ป้องกันการตั้งขาซ้ำซ้อน และป้องกันการใช้ขาที่สงวนไว้สำหรับ Flash / PSRAM ของ ESP32-S3",
      responsibilities: [
        "ตรวจสอบว่าขาที่เลือกเป็น GPIO ที่ปลอดภัย (Safe GPIO Check)",
        "ตรวจสอบว่าไม่มีขาใดถูกกำหนดให้ 2 อุปกรณ์พร้อมกัน (Conflict Detection)",
        "แปลงค่า Enums และชื่อขาพินสำหรับการแสดงผลบนหน้าเว็บ"
      ],
      safetyFeatures: ["Safe ESP32-S3 Pin Range Filtering", "ป้องกันฮาร์ดแวร์ช็อตจากการคอนฟิกขาพินชนกัน"],
      keyAPIs: ["hardware_map_validate()", "hardware_map_has_conflicts()", "hardware_map_default()"]
    },
    relay_safe_init: {
      layer: "layer_bus_core",
      id: "relay_safe_init",
      thaiName: "ตัวล็อกขารีเลย์ขณะเปิดเครื่อง (Relay Safe Initializer)",
      engName: "Glitch-Free Boot Driver",
      sourceFile: "firmware/components/relay_safe_init/relay_safe_init.c",
      rtosContext: { core: "Core 0", priority: "Pre-Init", stack: "Minimal", rate: "Boot Instant" },
      desc: "ไดรเวอร์เริ่มต้นขารีเลย์ทันทีในสเต็ปแรกสุดของ app_main เพื่อดึงสัญญาณให้อยู่ในสถานะปิด (Inactive) เสมอ ป้องกันการกระตุกของหน้าสัมผัสรีเลย์",
      responsibilities: [
        "ตัดสัญญาณขับรีเลย์ทั้งหมดทันทีที่ชิปเริ่มทำงาน",
        "ตั้งค่าขารีเลย์เป็น Output ในสถานะ Off ก่อนที่โมดูลอื่นจะเริ่มทำงาน",
        "รองรับการปรับค่าขารีเลย์ตามแผนผัง Hardware Map"
      ],
      safetyFeatures: ["Glitch-free Contact Clamping", "ป้องกันไฟกระชากเข้ามอเตอร์ปั๊มตอนเปิดเครื่อง"],
      keyAPIs: ["relay_safe_init_pins()", "relay_safe_configure()"]
    },
    session: {
      layer: "layer_bus_core",
      id: "session",
      thaiName: "ระบบความปลอดภัยและการยืนยันตัวตน (Session Security)",
      engName: "Token Auth & IP Binding",
      sourceFile: "firmware/components/session/session.c",
      rtosContext: { core: "Core 1", priority: "Thread-Safe", stack: "Heap", rate: "On HTTP Request" },
      desc: "ระบบบริหารจัดการ Session Token สำหรับการเข้าใช้งานเว็บ ควบคุมการตรวจสอบสิทธิ์ ผูก Token เข้ากับ IP ของผู้ใช้ และตัดการเชื่อมต่อเมื่อหมดเวลา",
      responsibilities: [
        "สร้าง Session Token แบบสุ่มความยาว 32 ไบต์ Hex เมื่อล็อกอินสำเร็จ",
        "ตรวจสอบความถูกต้องของ Token ทุกครั้งที่มีคำขอ API แก้ไขการตั้งค่า",
        "ผูก Token กับ Client IP เพื่อป้องกันการขโมย Token ข้ามเครื่อง",
        "หมดอายุอัตโนมัติเมื่อไม่มีการใช้งานเกิน 30 นาที (Idle Expiration)"
      ],
      safetyFeatures: ["Client IP Binding Guard", "Brute-force Throttling", "Invalidate All on Factory Reset"],
      keyAPIs: ["session_init()", "session_create()", "session_validate()", "session_destroy()", "session_invalidate_all()"]
    },

    // --- Layer 4: Network & UI ---
    wifi_manager: {
      layer: "layer_network_ui",
      id: "wifi_manager",
      thaiName: "ระบบจัดการเครือข่าย Wi-Fi (Wi-Fi Manager)",
      engName: "Concurrent AP + STA Manager",
      sourceFile: "firmware/components/wifi_manager/wifi_manager.c",
      rtosContext: { core: "Core 0", priority: "3 (Medium)", stack: "4,096 B", rate: "Event-driven" },
      desc: "ควบคุมไดรเวอร์ Wi-Fi ของ ESP-IDF ในโหมด AP และ STA พร้อมกัน ปล่อย Hotspot FishPump_Setup สำหรับตั้งค่า และเชื่อมต่อ Wi-Fi บ้านอัตโนมัติ",
      responsibilities: [
        "เปิด Access Point (AP) ตลอดเวลาสำหรับเข้าตั้งค่า (192.168.4.1)",
        "เชื่อมต่อ Wi-Fi Station (STA) และดึง IP จากเร้าเตอร์",
        "Auto-reconnect อัตโนมัติเมื่อสัญญาณหลุด พร้อมระบบ Exponential Backoff",
        "รองรับการสลับเข้าสู่ Recovery AP เมื่อกดปุ่มฉุกเฉิน"
      ],
      safetyFeatures: ["WIFI_PS_NONE ป้องกัน mDNS แพ็กเกจตกหล่น", "Auto Fallback สู่ AP Mode เสมอ"],
      keyAPIs: ["wifi_manager_init()", "wifi_manager_connect_sta()", "wifi_manager_get_ap_ip()", "wifi_manager_start_recovery_ap()"]
    },
    dns_server: {
      layer: "layer_network_ui",
      id: "dns_server",
      thaiName: "ระบบดักจับโดเมนเนม (Captive Portal DNS Server)",
      engName: "Captive Portal DNS Resolver",
      sourceFile: "firmware/components/dns_server/dns_server.c",
      rtosContext: { core: "Core 1", priority: "3 (Medium)", stack: "3,072 B", rate: "UDP 53 Listener" },
      desc: "เซิร์ฟเวอร์ DNS น้ำหนักเบา ดักฟังพอร์ต UDP 53 ทุกคำขอโดเมนเนมจากมือถือหรือคอมพิวเตอร์ที่ต่อ AP จะถูกตอบกลับด้วย IP 192.168.4.1 เพื่อเปิดหน้าตั้งค่าทันที",
      responsibilities: [
        "ดักจับคำขอ DNS Query จากอุปกรณ์ที่เชื่อมต่อกับ AP",
        "ตอบกลับด้วย IP ของ ESP32 AP ทันทีทุกคำขอ (Wildcard DNS Response)",
        "ช่วยให้อุปกรณ์มือถือเด้งหน้าต่างตั้งค่า (Captive Popup) อัตโนมัติ"
      ],
      safetyFeatures: ["Minimal Buffer UDP Parser", "Zero Dynamic Allocation เพื่อประหยัด RAM"],
      keyAPIs: ["dns_server_start()", "dns_server_stop()"]
    },
    web_server: {
      layer: "layer_network_ui",
      id: "web_server",
      thaiName: "เว็บเซิร์ฟเวอร์และเกตเวย์ข้อมูลสด (ESP-HTTPD & SSE Gateway)",
      engName: "REST API & SSE Event Pusher",
      sourceFile: "firmware/main/web_server.c",
      rtosContext: { core: "Core 1", priority: "5 (High)", stack: "10,240 B", rate: "Event-driven (Port 80)" },
      desc: "ESP-HTTPD Server ให้บริการ REST API endpoints, เสิร์ฟหน้าเว็บ Single Page App (Gzip Compressed) จาก Flash และสตรีมข้อมูลสดสู่เบราว์เซอร์ผ่าน SSE",
      responsibilities: [
        "เสิร์ฟไฟล์ Static Web (HTML, CSS, JS, Fonts) แบบ Gzip โดยตรงจาก Flash",
        "ให้บริการ REST API ควบคุมปั๊ม, ตั้งเวลา, แก้ไข Wi-Fi และทำ OTA Update",
        "สมัครรับข้อมูลจาก telemetry_hub (Observer Callback)",
        "แปลงข้อมูลสถานะเป็น JSON และ Push สู่เบราว์เซอร์ผ่าน SSE (/api/events)"
      ],
      safetyFeatures: ["HTTP Server Queue Health Monitor", "Token Authentication On Mutating Routes"],
      keyAPIs: ["web_server_start()", "web_server_telemetry_cb()", "web_server_queue_health_check()", "httpd_register_uri_handler()"]
    },
    browser_ui: {
      layer: "layer_network_ui",
      id: "browser_ui",
      thaiName: "แดชบอร์ดควบคุมบนเบราว์เซอร์ (Web Frontend SPA)",
      engName: "Browser Single-Page Application",
      sourceFile: "frontend-app / embedded static gzip",
      rtosContext: { core: "Client Device", priority: "Browser JS", stack: "Client RAM", rate: "Real-time" },
      desc: "หน้าเว็บ Single Page Application ที่รันบนเบราว์เซอร์ของผู้ใช้ รับข้อมูลสดผ่าน SSE สตรีม แสดงกราฟสถานะ และส่งคำสั่งควบคุมผ่าน HTTP POST",
      responsibilities: [
        "แสดงสถานะสดของปั๊มน้ำ 1/2, อุณหภูมิ, ระดับน้ำ และความจำระบบ",
        "เปิดหน้าต่างกดยืนยันการตั้งค่า (Staging Confirmation) ภายใน 180s",
        "ส่งคำสั่งควบคุม Manual Start/Stop และแก้ไขการตั้งค่าผ่าน REST APIs"
      ],
      safetyFeatures: ["Live Connection Indicator", "Warning Countdown Timer ขณะอยู่ในช่วง Staging"],
      keyAPIs: ["EventSource('/api/events')", "fetch('/api/settings/*')", "fetch('/api/login')"]
    }
  },

  connections: [
    // --- Telemetry & Sensor Loop ---
    { id: "c_float_to_pump", from: "hardware_sensors", to: "pump_control", label: "Float Switch GPIO State", category: "telemetry", scenarios: ["overview", "telemetry"], desc: "สัญญาณระดับน้ำจากสวิตช์ลูกลอยส่งเข้าสู่ State Machine ปั๊มน้ำ" },
    { id: "c_ds18b20_to_cooling", from: "hardware_sensors", to: "cooling_control", label: "1-Wire Temp Digital Data", category: "telemetry", scenarios: ["overview", "telemetry"], desc: "สัญญาณอุณหภูมิดิจิทัลส่งผ่าน 1-Wire บัสเข้าสู่ Task ควบคุมความเย็น" },
    { id: "c_pump_to_telem", from: "pump_control", to: "telemetry_hub", label: "TELEMETRY_EVT_PUMP / FLOAT", category: "telemetry", scenarios: ["overview", "telemetry"], desc: "ส่ง Event สถานะปั๊มและระดับน้ำเข้าสู่ Telemetry Hub (Pub/Sub)" },
    { id: "c_cooling_to_telem", from: "cooling_control", to: "telemetry_hub", label: "TELEMETRY_EVT_COOLING", category: "telemetry", scenarios: ["overview", "telemetry"], desc: "ส่ง Event อุณหภูมิและสถานะคอมเพรสเซอร์เข้าสู่ Telemetry Hub" },
    { id: "c_app_to_telem", from: "app_main", to: "telemetry_hub", label: "TELEMETRY_EVT_HEALTH", category: "telemetry", scenarios: ["overview", "telemetry"], desc: "ส่งสถิติสุขภาพเครื่อง (Heap, PSRAM, RSSI, Uptime) ทุก 5 วินาที" },
    { id: "c_telem_to_web", from: "telemetry_hub", to: "web_server", label: "Telemetry Callback (Observer)", category: "telemetry", scenarios: ["overview", "telemetry"], desc: "Hub เรียก Callback ของ web_server เพื่อส่งต่อข้อมูลสด" },
    { id: "c_web_to_browser_sse", from: "web_server", to: "browser_ui", label: "SSE Stream (/api/events)", category: "telemetry", scenarios: ["overview", "telemetry"], desc: "สตรีม JSON ผ่าน Server-Sent Events เข้าสู่เบราว์เซอร์แบบ Real-time" },

    // --- Control & Output ---
    { id: "c_pump_to_relay", from: "pump_control", to: "hardware_relays", label: "Active-Low Trigger (Pump 1/2)", category: "control", scenarios: ["overview", "telemetry"], desc: "ส่งสัญญาณลอจิกเปิด-ปิดรีเลย์ขับปั๊มน้ำ 1 และ 2" },
    { id: "c_cooling_to_relay", from: "cooling_control", to: "hardware_relays", label: "Active-Low Trigger (Chiller)", category: "control", scenarios: ["overview", "telemetry"], desc: "ส่งสัญญาณลอจิกเปิด-ปิดรีเลย์คอมเพรสเซอร์ทำความเย็น" },
    { id: "c_ui_to_led", from: "hardware_ui_task", to: "hardware_ui_buttons", label: "LED State Patterns", category: "control", scenarios: ["overview", "boot", "staging_rollback"], desc: "ขับสัญญาณไฟแสดงสถานะระบบ (ปกติ, รอคอนเฟิร์ม Staging, กู้คืน)" },

    // --- Boot & Safe Init ---
    { id: "c_app_to_safe_init", from: "app_main", to: "relay_safe_init", label: "0. Clamp Relays Inactive", category: "safety", scenarios: ["boot"], desc: "สั่งล็อกขารีเลย์ทั้งหมดให้ตัดไฟทันทีในสเต็ปแรกสุดเพื่อความปลอดภัย" },
    { id: "c_safe_init_to_relay", from: "relay_safe_init", to: "hardware_relays", label: "Glitch-Free Clamping", category: "safety", scenarios: ["boot"], desc: "บังคับขา GPIO ของรีเลย์เป็น Inactive ก่อนที่โมดูลอื่นจะเริ่มทำงาน" },
    { id: "c_app_to_nvs_init", from: "app_main", to: "nvs_store", label: "1. Init & Pop Restart Reason", category: "nvs", scenarios: ["boot"], desc: "เริ่มต้นระบบ NVS และอ่านสาเหตุการรีสตาร์ตรอบก่อนหน้า" },
    { id: "c_nvs_to_hwmap", from: "nvs_store", to: "hardware_map", label: "Load Active Pin Config", category: "nvs", scenarios: ["boot"], desc: "โหลดการกำหนดขาพิน GPIO จาก NVS มาตรวจสอบความถูกต้อง" },
    { id: "c_app_to_wdt", from: "app_main", to: "twdt", label: "2. Init TWDT (10s Timeout)", category: "safety", scenarios: ["boot"], desc: "เปิดการทำงานของระบบเฝ้าระวัง Task Watchdog Timer" },
    { id: "c_app_to_cooling_task", from: "app_main", to: "cooling_control", label: "Start Cooling Task", category: "control", scenarios: ["boot"], desc: "สั่งสตาร์ต FreeRTOS Task ของระบบควบคุมอุณหภูมิ" },
    { id: "c_app_to_pump_task", from: "app_main", to: "pump_control", label: "Start Pump Task", category: "control", scenarios: ["boot"], desc: "สั่งสตาร์ต FreeRTOS Task ของระบบควบคุมปั๊มน้ำ" },
    { id: "c_app_to_wifi", from: "app_main", to: "wifi_manager", label: "3. Init Wi-Fi (AP + STA)", category: "network", scenarios: ["boot"], desc: "เริ่มต้นระบบเครือข่าย Wi-Fi ทั้งโหมด Hotspot และ Station" },
    { id: "c_app_to_web", from: "app_main", to: "web_server", label: "4. Start HTTP Server", category: "network", scenarios: ["boot"], desc: "เปิดบริการเว็บเซิร์ฟเวอร์พอร์ต 80" },
    { id: "c_app_to_dns", from: "app_main", to: "dns_server", label: "5. Start Captive DNS (UDP 53)", category: "network", scenarios: ["boot"], desc: "เปิด Captive Portal DNS สำหรับดักจับอุปกรณ์ที่ต่อ AP" },
    { id: "c_app_to_ui_task", from: "app_main", to: "hardware_ui_task", label: "6. Start UI Task (Core 1)", category: "control", scenarios: ["boot"], desc: "เริ่มงานสแกนปุ่มกดและแสดงผลไฟ LED บน Core 1" },

    // --- Web Command & Staging Rollback ---
    { id: "c_browser_to_web_cmd", from: "browser_ui", to: "web_server", label: "HTTP POST Command / Config", category: "network", scenarios: ["overview", "staging_rollback"], desc: "ส่งคำสั่งปรับค่าการทำงานหรือตั้งเวลาผ่าน HTTP REST" },
    { id: "c_web_to_session", from: "web_server", to: "session", label: "Validate Token & Client IP", category: "safety", scenarios: ["staging_rollback"], desc: "ตรวจสอบสิทธิ์ผู้ใช้และผูก IP กับ Session Token" },
    { id: "c_web_to_nvs_staging", from: "web_server", to: "nvs_store", label: "Write Staging Config (stg_type>0)", category: "nvs", scenarios: ["staging_rollback"], desc: "บันทึกการตั้งค่าใหม่ลงพื้นที่ Staging และตั้งเวลา Rollback" },
    { id: "c_nvs_to_app_timer", from: "nvs_store", to: "app_main", label: "Trigger 180s Rollback Timer", category: "safety", scenarios: ["staging_rollback"], desc: "เริ่มนับเวลาถอยหลัง 180 วินาที หากไม่ยืนยันจะ Rollback อัตโนมัติ" },
    { id: "c_app_rollback_exec", from: "app_main", to: "nvs_store", label: "Execute Rollback on Timeout", category: "safety", scenarios: ["staging_rollback"], desc: "กู้คืนการตั้งค่าเดิมจาก Backup Slot และรีสตาร์ตเครื่องป้องกัน Brick" },

    // --- Hardware Recovery ---
    { id: "c_button_to_ui_task", from: "hardware_ui_buttons", to: "hardware_ui_task", label: "Button Press & Hold Scan", category: "safety", scenarios: ["overview", "boot"], desc: "สแกนระยะเวลากดปุ่ม Boot และ External Button" },
    { id: "c_ui_to_wifi_recovery", from: "hardware_ui_task", to: "wifi_manager", label: "Trigger Recovery AP (Hold 2-5s)", category: "safety", scenarios: ["overview"], desc: "บังคับเปิด Wi-Fi AP ฉุกเฉินเมื่อกดปุ่มค้าง 2-5 วินาที" },
    { id: "c_ui_to_factory_reset", from: "hardware_ui_task", to: "app_main", label: "Trigger Factory Reset (Hold >5s)", category: "safety", scenarios: ["overview"], desc: "ส่งสัญญาณล้างค่าโรงงานเมื่อกดปุ่มค้างเกิน 5 วินาที" },
    { id: "c_app_to_twdt_feed", from: "app_main", to: "twdt", label: "Feed WDT Keep-Alive (5s)", category: "safety", scenarios: ["overview", "telemetry"], desc: "ป้อนสัญญาณ Keep-Alive ทุก 5 วินาที เพื่อยืนยันว่าลูปหลักไม่ค้าง" }
  ],

  scenarios: [
    {
      id: "overview",
      thaiName: "🌐 ภาพรวมสถาปัตยกรรมทั้งระบบ (System Overview)",
      desc: "แสดงความสัมพันธ์และการเชื่อมต่อทั้งหมดระหว่างทุกคอมโพเนนต์ ทั้ง 4 เลเยอร์",
      highlightConnections: ["c_float_to_pump", "c_ds18b20_to_cooling", "c_pump_to_telem", "c_cooling_to_telem", "c_app_to_telem", "c_telem_to_web", "c_web_to_browser_sse", "c_pump_to_relay", "c_cooling_to_relay", "c_app_to_twdt_feed", "c_browser_to_web_cmd"]
    },
    {
      id: "boot",
      thaiName: "⚡ ลำดับการบูตและฮาร์ดแวร์ปลอดภัย (Boot & Safe Init)",
      desc: "ขั้นตอนการเปิดเครื่อง: สั่งล็อกรีเลย์ (Safe Clamping) ➔ โหลด NVS ➔ เริ่ม Task Watchdog ➔ รัน Tasks ➔ เริ่ม Wi-Fi และ Web Server",
      highlightConnections: ["c_app_to_safe_init", "c_safe_init_to_relay", "c_app_to_nvs_init", "c_nvs_to_hwmap", "c_app_to_wdt", "c_app_to_cooling_task", "c_app_to_pump_task", "c_app_to_wifi", "c_app_to_web", "c_app_to_dns", "c_app_to_ui_task"]
    },
    {
      id: "telemetry",
      thaiName: "🔄 วงจรสตรีมข้อมูลสดและเซนเซอร์ (Live Telemetry & Sensor Loop)",
      desc: "การอ่านเซนเซอร์ (Float Switch, DS18B20) ➔ ส่งเข้า Telemetry Hub (Pub/Sub) ➔ ผลักดันผ่าน Server-Sent Events (SSE) ตรงสู่หน้าเว็บ",
      highlightConnections: ["c_float_to_pump", "c_ds18b20_to_cooling", "c_pump_to_telem", "c_cooling_to_telem", "c_app_to_telem", "c_telem_to_web", "c_web_to_browser_sse", "c_pump_to_relay", "c_cooling_to_relay", "c_app_to_twdt_feed"]
    },
    {
      id: "staging_rollback",
      thaiName: "🛡️ คำสั่งเว็บและความปลอดภัย Zero-Brick (Web Command & Rollback)",
      desc: "การส่งคำสั่งเปลี่ยนการตั้งค่าผ่านเว็บ ➔ ตรวจ Session ➔ บันทึก Staging ➔ จับเวลา 180s หากไม่ยืนยันจะ Rollback คืนค่าเดิมอัตโนมัติ",
      highlightConnections: ["c_browser_to_web_cmd", "c_web_to_session", "c_web_to_nvs_staging", "c_nvs_to_app_timer", "c_ui_to_led", "c_app_rollback_exec"]
    }
  ]
};

if (typeof module !== 'undefined' && module.exports) {
  module.exports = { SYSTEM_MAP_DATA };
}
