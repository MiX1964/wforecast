from flask import Flask, render_template, url_for, session, redirect, flash, g, request
from flask.ext.script import Manager, Shell
from flask.ext.bootstrap import Bootstrap
from flask.ext.moment import Moment
from flask.ext.sqlalchemy import SQLAlchemy
from flask.ext.migrate import Migrate, MigrateCommand
from flask_googlemaps import GoogleMaps, Map
from datetime import datetime
from flask.ext.wtf import Form
from wtforms import StringField, SubmitField
from wtforms.validators import Required
import os
import requests

basedir = os.path.abspath(os.path.dirname(__file__)) 

app = Flask(__name__)
app.config['SECRET_KEY'] = 'you-will-not-be-able-to-guess-it'
app.config['SQLALCHEMY_DATABASE_URI'] = \
    'sqlite:///' + os.path.join(basedir, 'places.db')
app.config['SQLALCHEMY_COMMIT_ON_TEARDOWN'] = True

manager = Manager(app)
bootstrap = Bootstrap(app)
moment = Moment(app)
db = SQLAlchemy(app)
migrate = Migrate(app, db)
GoogleMaps(app)

def make_shell_context():
    return dict(app=app, db=db, User=User, Role=Role)	
manager.add_command("shell", Shell(make_context=make_shell_context))
manager.add_command('db', MigrateCommand)

class PlaceForm(Form):
    place = StringField('Place to get forecast of:',validators=[Required()])
    submit = SubmitField('Submit')

class Place(db.Model):
    __tablename__ = 'places'
    id = db.Column(db.Integer, primary_key=True)
    name = db.Column(db.String(30), nullable=False)
    country = db.Column(db.String(3), nullable=False)
    latitude = db.Column(db.Float)
    longitude = db.Column(db.Float)
    
    def __repr__(self):
        return '<Place %r>' % self.name

def insert_place(record):
    place = Place(id=record[0], name=record[1], country=record[2], 
                  latitude=record[3], longitude=record[4])
    db.session.add(place)

def get_places():
    return Place.query.all()   

def place_not_in_database(cityID):
    place = Place.query.filter_by(id=cityID).first()
    if place is None:
        return True      
    else:
        return False
   
def fetch_place(place):
    '''
    fetch_place(str) -> details or None

    Fetches place from OpenWeatherMap weather database
    Inserts place in database
    Returns record with place details as a 
    list [0=id,1=country,2=latitude, 3=longitude]if found 
    or None if not found
    '''
    url = "http://api.openweathermap.org/data/2.5/weather?q="+place+"& APPID=814e9b0949a47a464944923cc3773d3f1"
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
    get_place_details(str) -> details or None

    Retrieves place from database
    Returns record from database with place details as properties 
    {.id,.country, .latitude, .longitude} if found 
    or None if not found
    '''
    place = Place.query.filter_by(name=city).first()
    if place is None: return None      
    else: return place

def delete_place(id):
    db.session.delete(Place(id=id))    


@app.route('/', methods=['GET', 'POST'])
@app.route('/index', methods=['GET', 'POST'])
def index():
    form = PlaceForm()
    if form.validate_on_submit():
        session['place'] = form.place.data
        details = get_place_details(session.get('place'))
        if details is not None:
            session['place_not_found'] = False 
            session['place_details']=[details.id, details.name, details.country, details.latitude, details.longitude]
            message = '[db]: City: ' + details.name + ' Country: ' + details.country + ' Latitude: ' + str(details.latitude) + ' Longitude: ' + str(details.longitude) 
            flash(message)          
        else:
            flash('Place not found in database. Trying OpenWeatherMap web')
            details = fetch_place(session.get('place'))            
            if details is not None:
                session['place_not_found'] = False
                session['place_details'] = details
                message = '[web]: City: ' + details[1] + ' Country: ' + details[2] + ' Latitude: ' + str(details[3]) + ' Longitude: ' + str(details[4])
                flash(message) 
            else:           
                session['place_not_found'] = True
                flash('Place not found in database or web. Try a different place')       
        if details is not None: 
            return redirect(url_for('mapview'))
    return render_template('index.html', form=form, current_time=datetime.utcnow())

@app.route('/mapview')
def mapview():
    details = session['place_details']
    if details is not None:
        lat = details[3]
        lon = details[4]
        print "Lat: ",lat, " Lon: ",lon
        mymap = Map(
            identifier="view-side",
            lat=lat,
            lng=lon,
            zoom=12,
            markers=[(lat,lon)],
            style="width:600px;height:600px;margin:0"
        )
        return render_template('maptest.html',mymap=mymap)
    else: page_not_found()  
           
'''
    sndmap = Map(
        identifier="sndmap",
        lat=37.4419,
        lng=-122.1419,
        markers={"http://maps.google.com/mapfiles/ms/icons/green-dot.png":[(37.4419,-122.1419)],
                 "http://maps.google.com/mapfiles/ms/icons/blue-dot.png":[(37.4300,-122.1400)]}   
    )
'''
 

@app.errorhandler(404)
def page_not_found(e):
    return render_template('404.html'), 404

@app.errorhandler(500)
def internal_server_error(e):
    return render_template('500.html'), 500

if __name__ == '__main__':
    app.run(debug=True)
    
