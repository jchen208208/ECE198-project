from flask import Flask
from extensions import db

app = Flask(__name__)

# Configure the database
app.config["SQLALCHEMY_DATABASE_URI"] = "sqlite:///rehab.db"
app.config["SQLALCHEMY_TRACK_MODIFICATIONS"] = False
app.config["SECRET_KEY"] = "change-this-to-a-random-secret"

# Initialize SQLAlchemy
db.init_app(app)

# Import database models
import models

# Create database tables
with app.app_context():
    db.create_all()

# Staff site pages (routes.py)
from routes import site
app.register_blueprint(site)

if __name__ == "__main__":
    app.run(debug=True)