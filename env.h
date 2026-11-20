const char *ssid = "Velxio-GUEST";  // ถ้าใช้ใน Sim ห้ามเปลี่ยน
const char *password = ""; // ใน Sim ไม่ต้องใส่ password
const char *mqtt_host     = "mqttxxx.dsel.duckdns.org";  // <---<<< แก้ให้ตรงกับ vps ของตนเอง
// ข้อมูลสำหรับ Authenticate กับ MQTT Broker


const char *mqtt_username = "mqttusername";     // <---<<< แก้ให้ตรงกับ username  เช่น dse000 
const char *mqtt_password = "mqttuserpassword"; // <---<<< แก้ให้ตรงกับ userpass เช่น piglet1234 

// copy ไส้ใน ca.crt วาง Certificate ไว้ในระหว่างบรรทัด Begin กับ End
const char *test_certificate = R"EOF(
-----BEGIN CERTIFICATE-----
-----END CERTIFICATE-----
)EOF";
