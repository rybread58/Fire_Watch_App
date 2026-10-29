// Fetches local weather + sunrise/sunset from Open-Meteo (free, no API key)
// and sends a simplified condition to the watch.

var UNITS = 'fahrenheit'; // or 'celsius'

// Must match the Cond enum in main.c
var COND = { CLEAR: 0, PARTLY: 1, DRIZZLE: 2, RAIN: 3, SNOW: 4, STORM: 5 };

// WMO weather code -> condition. Overcast and fog show as partly cloudy.
function classify(code) {
  if (code === 0) return COND.CLEAR;
  if (code <= 3 || code === 45 || code === 48) return COND.PARTLY;
  if (code >= 51 && code <= 57) return COND.DRIZZLE;
  if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return COND.RAIN;
  if ((code >= 71 && code <= 77) || code === 85 || code === 86) return COND.SNOW;
  if (code >= 95) return COND.STORM;
  return COND.CLEAR;
}

// "2026-10-27T07:21" -> minutes since local midnight
function minutesOf(iso) {
  var hm = iso.substr(11, 5).split(':');
  return parseInt(hm[0], 10) * 60 + parseInt(hm[1], 10);
}

function fetchFor(lat, lon) {
  var url = 'https://api.open-meteo.com/v1/forecast' +
    '?latitude=' + lat.toFixed(3) + '&longitude=' + lon.toFixed(3) +
    '&current=temperature_2m,weather_code' +
    '&daily=sunrise,sunset&forecast_days=1&timezone=auto' +
    '&temperature_unit=' + UNITS;

  var req = new XMLHttpRequest();
  req.onload = function () {
    if (req.status !== 200) {
      console.log('Weather HTTP ' + req.status);
      return;
    }
    try {
      var data = JSON.parse(req.responseText);
      var msg = {
        TEMP: Math.round(data.current.temperature_2m),
        COND: classify(data.current.weather_code),
        SUNRISE: minutesOf(data.daily.sunrise[0]),
        SUNSET: minutesOf(data.daily.sunset[0])
      };
      console.log('Weather: ' + JSON.stringify(msg));
      Pebble.sendAppMessage(msg, null, function (e) {
        console.log('Send failed: ' + JSON.stringify(e));
      });
    } catch (err) {
      console.log('Weather parse error: ' + err);
    }
  };
  req.onerror = function () { console.log('Weather request failed'); };
  req.open('GET', url);
  req.send();
}

function fetchWeather() {
  navigator.geolocation.getCurrentPosition(
    function (pos) {
      localStorage.setItem('lastLoc', JSON.stringify([pos.coords.latitude, pos.coords.longitude]));
      fetchFor(pos.coords.latitude, pos.coords.longitude);
    },
    function (err) {
      console.log('Location error: ' + err.message);
      var last = localStorage.getItem('lastLoc');
      if (last) {
        var ll = JSON.parse(last);
        fetchFor(ll[0], ll[1]);
      }
    },
    { timeout: 15000, maximumAge: 10 * 60 * 1000 }
  );
}

Pebble.addEventListener('ready', fetchWeather);

Pebble.addEventListener('appmessage', function (e) {
  if (e.payload.REQUEST !== undefined) fetchWeather();
});
