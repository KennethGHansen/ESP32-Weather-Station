# ESP32 Weather Station
## Using the ESP32-S3 + SEN‑BME680 Environmental Monitor + additional components
### Temperature · Humidity · Pressure · Gas · Air Quality · Barometer & Storm Alerts

This project is the development of a functioning "Weather station" where data can be read on a local display and also remotely via WIFI/Webserver

This project will build on from my ESP32_SEN-BME680 repository and i will add additional components and functionalities.
For more details concering the the first running code using a ESP32-S3 in cunjunction with the Bosch SEN-BME680 board
Please see my repository ESP32_SEN-BME680.

@ version 0.7 I have implemented the JSON interface from ESP32 to the remote site where data is now updated. 
Further work will happen on the frontend via my repo: Weather-Station-Frontend
For practical reasons i am not using GitHubs internal deployment (Github Pages) anymore, but Cloudflare, so this 
repo will only serve as a backup platform.

---

## TODO list
- [X] Add display
- [X] Add ESP32 RTOS task management
- [X] Add push button
- [X] Add WiFi
- [ ] Optional: Add proximity activation
- [ ] Optional: Power optimation
- [ ] Optional: Add outside temperature/humidity sensor

More to come!

---

## Working revision
v0.7 - ESP32 Weather station running with public access on: https://weather-station.kghansen123.workers.dev/
v0.8 - Fixed: Bosch temperature measurement to be more accurate

---

## License / Attribution

- Bosch BME68x SensorAPI is BSD‑3‑Clause licensed
- MIT licence

---


