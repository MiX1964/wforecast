from flask import Flask, Markup, render_template, url_for, session, redirect, flash, g, request
from flask.ext.script import Manager, Shell
from flask.ext.bootstrap import Bootstrap
from flask.ext.moment import Moment
from flask.ext.sqlalchemy import SQLAlchemy
from flask.ext.migrate import Migrate, MigrateCommand
from flask.ext.wtf import Form
from flask_googlemaps import GoogleMaps, Map
from wtforms import StringField, SubmitField, BooleanField
from wtforms.validators import Required, ValidationError
from wtforms.widgets import CheckboxInput
from jinja2 import Template
from datetime import datetime
import os
import cgi
import requests
import urllib3
import certifi
import urllib3.contrib.pyopenssl
import decimal
import pygmaps
from geopy.geocoders import Nominatim

APP_ID = "814e9b0949a47a464944923cc3773d3f1"
API_KEY = "AIzaSyAhkU7hrlLH1gE2KuKCEpSK8pm-xi1Kxwo"
MLS_KEY = "test"

basedir = os.path.abspath(os.path.dirname(__file__)) 

urllib3.contrib.pyopenssl.inject_into_urllib3()

app = Flask(__name__)
app.config['SECRET_KEY'] = 'you-will-not-be-able-to-guess-it'
app.config['SQLALCHEMY_DATABASE_URI'] = 'sqlite:///' + os.path.join(basedir, 'places.db')
app.config['SQLALCHEMY_COMMIT_ON_TEARDOWN'] = True

manager = Manager(app)
bootstrap = Bootstrap(app)
moment = Moment(app)
db = SQLAlchemy(app)
migrate = Migrate(app, db)
GoogleMaps(app)

# set 2 decimals for Decimal numbers
decimal.getcontext().prec = 2

def make_shell_context():
    return dict(app=app, db=db, User=User, Role=Role)	

manager.add_command("shell", Shell(make_context=make_shell_context))
manager.add_command('db', MigrateCommand)

def _check_not_default(form, field):
    '''
    _check_not_default()
    
    Raise error if Search button pushed without having typed a place
    '''
    if field.data == "Place to get forecast":
        raise ValidationError("Need to fill in a place")  
    
class PlaceForm(Form):
    '''
    Form to select place to get forecast
    Either type a place or push button "Locate me"
    Possibility to selecte "Remember me" to memorise place
    '''
    place = StringField('Place',validators=[Required(message="A place is required"),_check_not_default])
    remember_me = BooleanField('Remember me', widget=CheckboxInput())
    search = SubmitField('Search')
    locate_me = SubmitField('Locate me')

class MapviewForm(Form):
    '''
    Form to visualise map
    Forecast button to show weather forecast
    '''
    forecast = SubmitField('Forecast')

class Place(db.Model):
    '''
    Database with table "places"
    Id is place id in OpenWeatherMap
    Country is acronym for country
    '''
    __tablename__ = 'places'
    id = db.Column(db.Integer, primary_key=True)
    name = db.Column(db.String(30), nullable=False)
    country = db.Column(db.String(3), nullable=False)
    latitude = db.Column(db.Float)
    longitude = db.Column(db.Float)
    
    def __repr__(self):
        return '<Place %r>' % self.name+' '+str(self.id)

def insert_place(record):
    '''
    insert_place(list)
    
    Insert place information (record) in database
    '''
    place = Place(id=record[0], name=record[1], country=record[2], 
                  latitude=record[3], longitude=record[4])
    db.session.add(place)

def get_places():
    '''
    get_places()
    
    Return all places stored in database
    '''
    return Place.query.all()   

def place_not_in_database(cityID):
    '''
    place_not_in_database(int) -> bool
    
    Checks if place defined by cityID is NOT in database
    '''
    place = Place.query.filter_by(id=cityID).first()
    if place is None:
        return True      
    else:
        return False
   
def fetch_place(place):
    '''
    fetch_place(str) -> list or None

    Fetches place from OpenWeatherMap weather database
    Inserts place in database
    Returns record with place details as a 
    list [0=id,1=country,2=latitude, 3=longitude]if found 
    or None if not found
    '''
    url = "http://api.openweathermap.org/data/2.5/weather?q=" +  place + "& APPID=" + APP_ID
    response = requests.get(url)
    results = response.json()
    if results['cod'] != '404':
        rec = []
        rec.append(results['id'])
        rec.append(results['name'])
        rec.append(results['sys']['country'])
        rec.append(float(results['coord']['lat']))
        rec.append(float(results['coord']['lon']))
        if place_not_in_database(rec[0]): # check for ID already existing
            insert_place(rec)
#            db.session.commit()
        return rec
    else: return None

def get_place_details(city):
    '''
    get_place_details(str) -> dict or None

    Retrieves place from database
    Returns record from database with place details as properties 
    {.id,.country, .latitude, .longitude} if found 
    or None if not found
    '''
    place = Place.query.filter_by(name=city).first()
    if place is None: return None      
    else: return place

def delete_place(id):
    '''
    delete_place(int)
    
    Delete place with cityID = id from database
    '''
    db.session.delete(Place(id=id))    

def _boolean(bvalue):
    '''
    _boolean(bool) -> str
    
    Returns string with value of bool
    '''
    if bvalue: return "True"
    else: return "False" 
    
def _toCelsius(tKelvin):
    '''
    _toCelsius(float) -> float (1 decimal)
    
    Returns temp in Celsius
    '''
    if tKelvin:
        return round(tKelvin-273,1)
    else: return ''  
    
def _toBeaufort(wSpeed):
    '''
    _toBeaufort(float) -> int
    
    Returns wind speed in Beaufort scale
    
    wSpeed wind speed in m/s
    wSpeed = 0.837 B^(3/2)
    Beaufort scale is 0-12
    '''
    return (int(round(pow(float(wSpeed/0.837),(2.0/3.0)))))
    
def _toWindRose(wDirectionDeg):
    '''
    _toWindRose(int) -> int
    
    Returns wind direction in segments of wind rose (22.5 deg)
    '''
    return (int(round((wDirectionDeg // 22.5)* 22.5)))

def decodeWeatherRecord(record):
    '''
    decodeWeatherRecord(dict) -> list
    
    Returns list with parameter from JSON record
    0: date
    1: time
    2: day/night
    3: weather 
    4: temp
    5: atmospheric pressure
    6: humidity %
    7: wind speed
    8: wind direction
    9: rain mm in 3h
    '''   
    if 'description' in record['weather'][0]: 
        weather = record['weather'][0]['id'] 
    else: 
        weather = ''    
    if 'pressure' in record['main']: 
        pressure = int(record['main']['pressure']) 
    else: 
        pressure = ''  
    if 'humidity' in record['main']: 
        humidity = int(record['main']['humidity']) 
    else: 
        humidity = ''
    if 'speed' in record['wind']: 
        windSpeed = _toBeaufort(record['wind']['speed']) 
    else: 
        windSpeed = ''
    if 'deg' in record['wind']:  
        windDirection = _toWindRose(record['wind']['deg']) 
    else: 
        windDirection = ''
    if 'rain' in record:
        if '3h' in record['rain']:
            rain = round(record['rain']['3h'],2)
        else:
            rain = ''
    else:
        rain = ''
    hour = int(record['dt_txt'][10:13])
    if hour > 6 and hour < 21: day = True 
    else: day = False
    if hour > 12: hour -= 12
    if hour == 0: hour = 12
    forecast = [record['dt_txt'][8:10]+'/'+record['dt_txt'][5:7],
                hour,
                day,
                weather,
             	   _toCelsius(record['main']['temp']),
              	  pressure,
              	  humidity,
              	  windSpeed,
              	  windDirection,
              	  rain]
    return forecast

def fetch_wForecast(id):
    '''
    fetch_wForecast(int) -> dict
    
    Read weather forecast from OpenWeatherMap
    for city with id
    '''
    url = 'http://api.openweathermap.org/data/2.5/forecast?id='+\
           str(id)+'&APPID='+APP_ID  
    response = requests.get(url)    
    results = response.json()
    if results['cod'] != '404':
        record = {}
        if 'name' in results['city']: 
            record['city'] = results['city']['name'] 
        else:
            record['city'] = ''
        if 'country' in results['city']:
            record['country'] = results['city']['country'] 
        else: 
            record['country'] = ''
        print "City: ",record['city']," (",record['country'],")"    
        rlist = results['list']
        record['forecasts']=[]
        first = True
        for index in range(0,len(rlist)):
            single_forecast = rlist[index]
            if first: 
                record['pressure'] = int(single_forecast['main']['pressure'])
                first = False
            record['forecasts'].append(decodeWeatherRecord(single_forecast))
        return record
    else: return None

def fetch_weather(city):
    '''
    fetch_weather(int) -> dict
    
    Read weather for city
    '''
    url = 'http://api.openweathermap.org/data/2.5/weather?q='+\
        city+'&APPID='+APP_ID
    response = requests.get(url)
    results = response.json()
    if results['cod'] != '404':
        record = {}
        if 'description' in results['weather'][0]: 
            record['weather'] = results['weather'][0]['description'] 
        else: 
            record['weather'] = ''
        if 'temp' in results['main']:
            record['temperature'] = _toCelsius(results['main']['temp'])
        else: 
            record['temperature'] = ''
        if 'pressure' in results['main']: 
            record['pressure'] = results['main']['pressure'] 
        else: 
            record['pressure'] = ''
        if 'humidity' in results['main']:
            record['humidity'] = results['main']['humidity'] 
        else: 
            record['humidity'] = ''
        if 'speed' in results['wind']: 
            record['wind_speed'] = results['wind']['speed'] 
        else: 
            record['wind_speed'] = ''
        if 'deg' in results['wind']: 
            record['wind_direction'] = results['wind']['deg'] 
        else: 
            record['wind_direction'] = ''
        return record
    else: return None

def find_location_details(position):
    '''
    find_location_details(dict) -> dict
    
    Look for address of position (defined as latitude, longitude)
    Return dict with city, country code, latitude and longitude
    ''' 
    address = {}
    # geolocate position
    geolocator = Nominatim()
    location = geolocator.reverse((position['latitude'],position['longitude']))
    if 'address' in location.raw:
        location_details = location.raw['address']
        address['city'] = location_details['town']
        address['country'] = location_details['country_code']
        address['latitude'] = location.raw['lat']
        address['longitude'] = location.raw['lon']
    return address
        
def find_current_location():
    '''
    find_current_location() -> dict
    
    Get current location using Mozilla location services (geolocate)
    Returns dict with latitude, longitude and accuracy
    '''
    url = 'https://location.services.mozilla.com/v1/geolocate?key=' + MLS_KEY
    response = requests.get(url)
    print "@find_current_location: response = ", response
    results = response.json()
    location = {}
    location['latitude'] = results['location']['lat']
    location['longitude'] = results['location']['lng']
    location['accuracy'] = results['accuracy']
    print "@find_current_location: results = ", results
    print "@find_current_location: location = ", location
    return location

def find_weather_stations_near(location):
    '''
    find_weather_stations_near(dict) -> list of dict
    
    Looks for weather stations around location (defined as latitude, longitude)
    Returns a list of stations defined by a dict (id, latitude, longitude)
    '''
    url = 'http://api.openweathermap.org/data/2.5/station/find?lat='+location['latitude']+\
           '&lon='+location['longitude']+'cnt=1'+'&APPID='+APP_ID
    response = requests.get(url)
    results = response.json()
    stations = []
    for number in range(len(results)):
        record = results[number]
        station = {}        
        if 'station' in record:
            station['id'] = record['station']['id']
            station['latitude'] = record['station']['coord']['lat']
            station['longitude'] = record['station']['coord']['lon']
            stations.append(station)
            print "[find_weather_stations_near] station[%i] id:%s @ (%s,%s)" % (number,station['id'],station['latitude'],station['longitude'])
        else:
            print "[find_weather_stations_near] No weather station found in record %i near given location" % (number) 
    if stations:
        session['station'] = stations[0]
    else:
        session['station'] = None    
    return stations
 
def get_current_location():
    '''
    get_current_location() -> dict
    
    Latitude, longitude and accuracy are stored in a form (from XMLHttpRequest)
    City in (lat, long) retrieved through reverse geolocation
    get_current_location()-> position:
        latitude
        longitude
        accuracy (in m)
    and get details (city) through reverse geolocation
    find_location_details(position) -> location:
        city
        country
        latitude
        longitude     
    '''
    position = {}    
    position['latitude'] = request.form.get('lat',None)
    position['longitude'] = request.form.get('lon',None)
    position['accuracy'] = request.form.get('acc',None)
    if position['latitude']: 
        session['XHttpRequest_location'] = True
        print "[get_current_location] Lat: ", position['latitude']
        print "[get_current_location] Lon: ", position['longitude']
        print "[get_current_location] Acc: ", position['accuracy']
        # geolocate current location
        location = find_location_details(position) 
        if location:  
            city = location['city']
            print "[get_current_location] City: ", city
            details = get_place_details(city)
            if details is not None: # city is already in database
                print "[get_current_location] City already in database"
                session['place_found'] = True 
                session['place_details']=[details.id, details.name, details.country, details.latitude, details.longitude]
                print "[get_current_location] Details: City: " + details.name + ' Country: ' + details.country + ' Latitude: ' + str(details.latitude) + ' Longitude: ' + str(details.longitude)         
            else: # try to find a weather station by city
                print "[get_current_location] City not in database, trying OpenWeatherMap web"
                details = fetch_place(session.get('place'))            
                if details is not None: # found a weather station by city
                    print "[get_current_location] Found a weather station in city"
                    session['place_found'] = True
                    session['place_details'] = details
                    print "[get_current_location] Details: City: " + details.name + ' Country: ' + details.country + ' Latitude: ' + str(details.latitude) + ' Longitude: ' + str(details.longitude)         
                else: # not found in OpenWeatherMap, search for nearest station 
                    print "[get_current_location] Not found in OpenWeatherMap, search for nearest station" 
                    geo_location = {}
                    geo_location['latitude'] = location['latitude']
                    geo_location['longitude'] = location['longitude']
                    stations = find_weather_stations_near(geo_location)
                    details = []
                    if stations:
                        print "[get_current_location] Found a weather station based on city geo location"
                        session['place_found'] = True
                        details.append(session['station']['id'])
                        details.append(city)
                        details.append(location['country'])
                        details.append(session['station']['latitude'])
                        details.append(session['station']['longitude'])
                        session['place_details'] = details
                        print "[get_current_location] Details: City: " + details.name + ' Country: ' + details.country + ' Latitude: ' + str(details.latitude) + ' Longitude: ' + str(details.longitude)         
                        # store place in database
                        insert_place(details)
                    else:
                        print "[get_current_location] City not found by any means"
                        session['place_found'] = False
                        flash('Place not found in database or web. Try a different place') 
                        position = None
    else: 
        session['XHttpRequest_location'] = False 
    return position

def show_map(location):
    '''
    show_map(dict) -> str (HTML with map)
    
    Use pygmaps (as a class modified in this project) to draw map
    and overlay icon on location (latitude, longitude) 
    and a circle with the accuracy
    Overlay the weather stations near location
    Generates an HTML file that is returned
    '''
    zoom = 15    
    mymap = pygmaps.maps(float(location['latitude']),float(location['longitude']),zoom)
    mymap.addpoint(float(location['latitude']),float(location['longitude']),'house')
    mymap.addcircle(float(location['latitude']),float(location['longitude']),float(location['accuracy']),"#0000FF")
    # add nearest weather stations
    stations = find_weather_stations_near(location)
    for number in range(len(stations)):
        station = stations[number]    
        if station:
            mymap.addpoint(float(station['latitude']), float(station['longitude']), 'station')
        else:
            print "[show_map] No weather station to display"
    mymap.draw('./templates/mymap.html')
    f = open('./templates/mymap.html','r')
    fmap = Markup(f.read())
    return fmap
    
@app.route('/', methods=['GET', 'POST'])
@app.route('/index', methods=['GET', 'POST'])
def index():
    '''
    Show initial form
    In first instance gets location using XMLHttpRequest 
    get_current_location()-> position:
        latitude
        longitude
        accuracy (in m)
    and get details (city) through reverse geolocation
    find_location_details(position) -> location:
        city
        country
        latitude
        longitude
    find weather station for city:
        1) if city in database retrieve details including weather station id
        2) if city in OpenWeatherMap weather web retrieve station id
        3) find weather stations near geo location and select nearest
    2 buttons:
            1) locate_me: information retrieved from geolocation is stored 
               in location in session. Redirect to showMap
            2) search: place (with possibility of remembering it: remember_me)
               looks for details:
                   a) first in database, with get_place_details(place) 
                   b) second in the OpenWeatherMap weather web, fetch_place(place)
               if details are found, redirect to mapView
    '''
    print "[index]"
    form = PlaceForm()
    if 'locate_me' in request.form: # Locate me button pressed
        print "@index: Locate me button pressed"
        position = session['location']
        print "[index] position: ", position
        if position:
            print "[index] position given, redirect to showMap"
            return redirect(url_for('showMap'))
        else:
            print "[index] position not given, continue in index"
    elif form.validate_on_submit():
        if 'search' in request.form:
            print "@index: Search button pressed"
            session['place_selected'] = True
            session['place'] = form.place.data
            session['remember_place'] = form.remember_me.data
            details = get_place_details(session.get('place'))
            if details is not None:
                session['place_found'] = True 
                session['place_details']=[details.id, details.name, details.country, details.latitude, details.longitude]         
            else:
                details = fetch_place(session.get('place'))            
                if details is not None:
                    session['place_found'] = True
                    session['place_details'] = details
                else:           
                    session['place_found'] = False
                    flash('Place not found in database or web. Try a different place') 
                    return redirect(url_for('index'))  
            return redirect(url_for('mapView'))
    else:
        # no button pressed in the form
        # read the XMLHttpRequest parameters
        position = get_current_location()  
        if not position: return redirect(url_for('index'))   
        session['location'] = position     
        print "[index] position: ", position              
    if session.get('remember_place'):  
        session['map_shown'] = False        
        form.place.data = session.get('place')
        form.remember_me.data = True
        session['place_found'] = True
        session['place_selected'] = False
    else:
        session['map_shown'] = False
        form.place.data = "Place to get forecast"
        form.remember_me.data = False 
    print "@index: Place: ", form.place.data
    print "@index: Remember_me: ", _boolean(form.remember_me.data)
    return render_template('index.html', form=form)

@app.route('/showMap', methods=['GET', 'POST'])
def showMap():
    '''
    Show map for found location (locate me)
    Map (as HTML) is stored in location in session:
    View is redirected to forecast when the button is pressed in the form
    '''
    print "[showMap]"
    form = MapviewForm()
    if 'forecast' in request.form: # Forecast button pressed
        print "[showMap] Forecast button pressed"
        session['place_found'] = True        
        return redirect(url_for('forecast'))    
    session['mymap'] = show_map(session['location'])
    return render_template('showmap.html',
                           form=form,
                           location=session['location'],
                           mymap=session['mymap'])

@app.route('/mapView', methods=['GET', 'POST'])
def mapView():
    '''
    Show map and current weather for selected location
    Details are stored in place_details in session:
         [0]
         [1]:   city
         [2]
         [3]:   latitude
         [4]:   longitude
    Current weather is retrieved by fetch_weather(city)
    View is redirected to forecast when the button is pressed in the form
    '''
    print "[mapView]"
    form = MapviewForm()
    details = session.get('place_details')           
    if not session.get('map_shown') and details is not None:
        print "[mapView] Map not shown + details OK" 
        session['map_shown']= True
        lat = details[3]
        lon = details[4]
        mymap = Map(identifier="view-side",
                    lat=lat,
                    lng=lon,
                    zoom=12,
                    markers=[(lat,lon)],
                    style="width:680px;height:280px;margin:0")
        session['mymap'] = mymap        
        current_weather = fetch_weather (details[1])
        session['current_weather'] = current_weather     
        print "[mapView] Current_weather: ", current_weather
    if 'forecast' in request.form:
        print "[mapView] Forecast button pressed"                    
        return redirect(url_for('forecast')) 
    return render_template('mapview.html', 
                           form=form,
                           place=details[1],
                           country=details[2],
                           mymap=session['mymap'],
                           weather=session['current_weather'])  

@app.route('/forecast', methods=['GET', 'POST'])
def forecast():
    '''
    Show weather forecast for selected location
    Location is either 
        1) found (place_found in session) or 
        2) remembered (remember_place in session)
    In both cases location details ([0]: id - weather station id) 
    are stored in place_details in session
    Weather forecast is retrieved by fetch_wForecast(id)
    Forecast is rendered as a table
    '''
    print "[forecast]"
    print "remember_place:", _boolean(session.get('remember_place'))
    print "place_found:", _boolean(session.get('place_found'))
    if session.get('remember_place') or session.get('place_found'):
        details = session.get('place_details')
    else:
        details = None
        session['place_found'] = False
        session['place_selected'] = False
        session['place'] = None
        session['place_details'] = None
        session['remember_place'] = False
        session['current_weather'] = None
        flash('Place not selected. Please select a place prior to requesting forecast')
        return redirect(url_for('index'))
    if details is not None:								        
        wForecast_record = fetch_wForecast(details[0])
        if wForecast_record is not None:        
            return render_template('forecast.html', 
                                   forecast=wForecast_record, 
                                   current_time=datetime.utcnow())
        else: page_not_found()
    else: page_not_found()
           
@app.errorhandler(404)
def page_not_found(e):
    return render_template('404.html'), 404

@app.errorhandler(500)
def internal_server_error(e):
    return render_template('500.html'), 500

if __name__ == '__main__':
    session={}
#    app.run(debug=True)
    app.run(host='0.0.0.0')
    
