# ESP32 Ruuvi Collector configuration file formats

Here is the specification of the configuration files that the ESP32 Ruuvi Collector uses.

## influxdb.txt

row 1: the InfluxDB write url 

row 2: username:password for the url. They must be separated by colon.

row 3: measurement name (set to ruuvi_measurements for compatibility)

row 4: post interval in minutes. 0 = all the time, practically approximately every 10s.

All rows must end in newline.

**Example influxdb.txt file:**

```
https://influxdb.some.where:8086/write?db=ruuvi
ruuvi:password123
ruuvi_measurements
0
```

## known_tags.txt

One known tag per row. First the MAC address in lowercase hex and colons between bytes, then TAB, 
then name of the tag and newline.

**Example known_tags.txt file:**

```
f4:01:83:12:ce:95	foo
e3:28:8c:99:47:ae	bar
```

## known_wifis.txt

One known WiFi network per row. First the SSID, then TAB, then password and newline.

**Example known_wifis.txt**

```
OH2MP	MyVerySecretPass123
OH2MP-5	AnotherVerySecretPass456
```
