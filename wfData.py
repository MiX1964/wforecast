'''
wfData

Data module for Weather Forecast
'''


from flask.ext.sqlalchemy import SQLAlchemy
#import sqlite3 as sql
import requests

if __name__ == "__main__":
    from flask import Flask
    import os
    basedir = os.path.abspath(os.path.dirname(__file__)) 
    app = Flask(__name__)

db = SQLAlchemy(app)
app.config['SQLALCHEMY_DATABASE_URI'] = \
               'sqlite:///' + os.path.join(basedir, 'places.db')
app.config['SQLALCHEMY_COMMIT_ON_TEARDOWN'] = True

'''
db = None
cursor = None
'''

class Place(db.Model):
    __tablename__ = 'places'
    id = db.Column(db.Integer, primary_key=True)
    name = db.Column(db.String(30), nullable=False)
    country = db.Column(db.String(3), nullable=False)
    latitude = db.Column(db.Float)
    longitude = db.Column(db.Float)
    
    def __repr__(self):
        return '<Place %r>' % self.name

def insert_place(ID, Name, Country, Latitude, Longitude):
    place = Place(id=ID, name=Name, country=Country, 
                  latitude=Latitude, longitude=Longitude)
    db.session.add(place)
'''
    query = "insert into places(ID, Name, Country, Latitude, Longitude)values (?, ?, ?, ?, ?)"
    cursor.execute(query, (ID, Name, Country, Latitude, Longitude))
'''
def get_places():
    return Place.query.all()   
'''
    query = "select ID, Name, Country, Latitude, Longitude from places"
    return cursor.execute(query).fetchall()   
'''

def place_not_in_database(cityID):
    place = Place.query.filter_by(id=cityID).first()
    if place is None:
        print "[place_not_in_database]: %s not in database" % cityID
        return True      
    else:
        return False
'''
    query = "select Name from places where ID = ?"
    try:
        record = cursor.execute(query, (cityID,)).fetchall()
        if record: return False
        else: return True
    except:
        print "(place_not_in_database) : Error in accessing database",city
        return False
'''
   
def fetch_place(place):
    '''
    fetch_place(str) -> details or None

    Fetches place from OpenWeatherMap weather database
    Inserts place in data base
    Returns record with place details if found or None if not found
    '''
    url = "http://api.openweathermap.org/data/2.5/weather?q="+place+"& APPID=814e9b0949a47a464944923cc3773d3f1"
    response = requests.get(url)
    results = response.json()
    if results['cod'] != '404':
        rec = []
        place = results['name']
        rec.append(results['id'])
        rec.append(results['sys']['country'])
        rec.append(float(results['coord']['lat']))
        rec.append(float(results['coord']['lon']))
        if place_not_in_database(rec[0]): # check for ID already existing
            insert_place(rec[0],place,rec[1],rec[2],rec[3])
        return rec
    else:
        print "Not found"
        print results
        return None

def get_place_details(city):
    place = Place.query.filter_by(name=city).first()
    if place is None:
        print "[get_place_details]: %s not in database" % city
        return None      
    else:
        return place
'''
    query = "select ID, Country, Latitude, Longitude from places where Name = ?"
    try:
        record = cursor.execute(query, (city,)).fetchall()
        if record: return record[0]
        else:
            return None
    except:
        print "(get_place_details) : Error in accessing database",city
        return None    
'''

def delete_place(id):
    db.session.delete(Place(id=id))    
'''
    query = "delete from places where ID = ?"
    cursor.execute(query,(id,))
'''

'''
def initDB(filename=None):
    global db, cursor
    if not filename:
        filename = "places.db"
    try:
        db = sql.connect(filename)
        cursor = db.cursor()
    except:
        print"Error connecting to ", filename
        cursor = None
        raise
'''
   
def commitDB():
    db.session.commit()
'''
    try:
        cursor.close()        
        db.commit()
    except:
        print "Problem committing database..."
        raise
'''

def closeDB():
    db.session.commit()
'''
    try:
        cursor.close()
        db.commit()
        db.close()
    except:
        print "Problem closing database..."
        raise
'''

def _test_city_search(city):
     print "\n[get_place_details(city)]"
     print "City: ", city
     record = get_place_details(city)
     if record:
        print "ID: ", record.id, " Country: ",record.country, " Lat: ",record.latitude, " Lon: ",record.longitude
     else:
        print "City not found in database"

def _test_city_fetch(city):
     print "\n[fetch_place(city)]"
     print "City: ", city
     record = fetch_place(city)
     if record:
        print "ID: ", record[0], " Country: ",record[1], " Lat: ",record[2], " Lon: ",record[3]
     else:
        print "City not found in OpenWeatherMap web"
        
if __name__ == "__main__":
    print "Places:\n", get_places()
    """
    _test_city_search(u'Barcelona')
    _test_city_search(u'Madrid')
    _test_city_search(u'NotInDatabase')
    _test_city_fetch(u'Barcelona')
    _test_city_fetch(u'Madrid')
    _test_city_search(u'Madrid')
    _test_city_fetch(u'NotInOpenWeatherMapWeb')
    """


