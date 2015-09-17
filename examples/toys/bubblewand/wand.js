//  wand.js
//  part of bubblewand
//
//  Script Type: Entity Script
//  Created by James B. Pollack @imgntn -- 09/03/2015
//  Copyright 2015 High Fidelity, Inc.
//
//  Makes bubbles when you wave the object around.
//  
//  For the example, it's attached to a wand -- but you can attach it to whatever entity you want.  I dream of BubbleBees :) bzzzz...pop!
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html

'use strict';

(function() {

  Script.include("../../utilities.js");
  Script.include("../../libraries/utils.js");

  var BUBBLE_MODEL = "http://hifi-public.s3.amazonaws.com/james/bubblewand/models/bubble/bubble.fbx";
  var BUBBLE_SCRIPT = 'http://hifi-public.s3.amazonaws.com/james/bubblewand/scripts/bubble.js?' + randInt(1, 10000);
  Script.resolvePath('bubble.js?' + randInt(0, 5000));

  var HAND_SIZE = 0.25;
  var TARGET_SIZE = 0.04;



  var BUBBLE_GRAVITY = {
    x: 0,
    y: -0.1,
    z: 0
  }

  var WAVE_MODE_GROWTH_FACTOR = 0.005;
  var SHRINK_LOWER_LIMIT = 0.02;
  var SHRINK_FACTOR = 0.001;
  var VELOCITY_STRENGTH_LOWER_LIMIT = 0.01;
  var BUBBLE_DIVISOR = 50;
  var BUBBLE_LIFETIME_MIN = 3;
  var BUBBLE_LIFETIME_MAX = 8;

  function getGustDetectorPosition() {
    //put the zone in front of your avatar's face
    var DISTANCE_IN_FRONT = 0.2;
    var DISTANCE_UP = 0.5;
    var DISTANCE_TO_SIDE = 0.0;

    var up = Quat.getUp(MyAvatar.orientation);
    var front = Quat.getFront(MyAvatar.orientation);
    var right = Quat.getRight(MyAvatar.orientation);

    var upOffset = Vec3.multiply(up, DISTANCE_UP);
    var rightOffset = Vec3.multiply(right, DISTANCE_TO_SIDE);
    var frontOffset = Vec3.multiply(front, DISTANCE_IN_FRONT);

    var offset = Vec3.sum(Vec3.sum(rightOffset, frontOffset), upOffset);
    var position = Vec3.sum(MyAvatar.position, offset);
    return position;
  }


  var wandEntity = this;

  this.preload = function(entityID) {
    this.entityID = entityID;
    this.properties = Entities.getEntityProperties(this.entityID);
    BubbleWand.originalProperties = this.properties;
    BubbleWand.init();
    print('initial position' + JSON.stringify(BubbleWand.originalProperties.position));
  }

  this.unload = function(entityID) {
    Entities.editEntity(entityID, {
      name: ""
    });
    Script.update.disconnect(BubbleWand.update);
    Entities.deleteEntity(BubbleWand.currentBubble);
    while (BubbleWand.bubbles.length > 0) {
      Entities.deleteEntity(BubbleWand.bubbles.pop());
    }

  };


  var BubbleWand = {
    bubbles: [],
    timeSinceMoved: 0,
    resetAtTime: 2,
    currentBubble: null,
    atOriginalLocation: true,
    update: function(deltaTime) {
      BubbleWand.internalUpdate(deltaTime);
    },
    internalUpdate: function(deltaTime) {
      var _t = this;

      var GRAB_USER_DATA_KEY = "grabKey";
      var defaultGrabData = {
        activated: false,
        avatarId: null
      };

      var grabData = getEntityCustomData(GRAB_USER_DATA_KEY, wandEntity.entityID, defaultGrabData);
      if (grabData.activated && grabData.avatarId == MyAvatar.sessionUUID) {

        // remember we're being grabbed so we can detect being released
        _t.beingGrabbed = true;

        // print out that we're being grabbed
        // print("I'm being grabbed...");
        _t.handleGrabbedWand();

      } else if (_t.beingGrabbed) {

        // if we are not being grabbed, and we previously were, then we were just released, remember that
        // and print out a message
        _t.beingGrabbed = false;
        // print("I'm was released...");
        return
      }

    },
    handleGrabbedWand: function() {
      var _t = this;

      // print('HANDLE GRAB 1')
      var properties = Entities.getEntityProperties(wandEntity.entityID);
      var wandPosition = properties.position;
 
      var velocity = Vec3.subtract(wandPosition, _t.lastPosition)

      // print('VELOCITY:' + JSON.stringify(velocity));
      // print('HANDLE GRAB 2')
      var velocityStrength = Vec3.length(velocity) * 100;

      var upVector = Quat.getUp(properties.rotation);
      var frontVector = Quat.getFront(properties.rotation);
      var upOffset = Vec3.multiply(upVector, 0.2);
      var wandTipPosition = Vec3.sum(wandPosition, upOffset);
      _t.wandTipPosition = wandTipPosition;

      if (velocityStrength < VELOCITY_STRENGTH_LOWER_LIMIT) {
        velocityStrength = 0
      }

      var isMoving;
      if (velocityStrength === 0) {
        isMoving = false;
      } else {
        isMoving = true;
      }
      // print('MOVING?' + isMoving)
      if (isMoving === true) {
        _t.timeSinceMoved = 0;
        _t.atOriginalLocation = false;
      } else {
        _t.timeSinceMoved = _t.timeSinceMoved + deltaTime;
      }

      if (isMoving === false && _t.atOriginalLocation === false) {
        if (_t.timeSinceMoved > _t.resetAtTime) {
          _t.timeSinceMoved = 0;
          _t.returnToOriginalLocation();
        }
      }


      // default is 'wave mode', where waving the object around grows the bubbles

      //store the last position of the wand for velocity calculations
      _t.lastPosition = wandPosition;

      if (velocityStrength > 10) {
        velocityStrength = 10
      }

      //actually grow the bubble
      var dimensions = Entities.getEntityProperties(_t.currentBubble).dimensions;

      if (velocityStrength > 1) {

        //add some variation in bubble sizes
        var bubbleSize = randInt(1, 5);
        bubbleSize = bubbleSize / BUBBLE_DIVISOR;

        //release the bubble if its dimensions are bigger than the bubble size
        if (dimensions.x > bubbleSize) {
          //bubbles pop after existing for a bit -- so set a random lifetime
          var lifetime = randInt(BUBBLE_LIFETIME_MIN, BUBBLE_LIFETIME_MAX);

          Entities.editEntity(_t.currentBubble, {
            velocity: velocity,
            lifetime: lifetime
          });

          //release the bubble -- when we create a new bubble, it will carry on and this update loop will affect the new bubble
          BubbleWand.spawnBubble();

          return
        } else {

       
            dimensions.x += WAVE_MODE_GROWTH_FACTOR * velocityStrength;
            dimensions.y += WAVE_MODE_GROWTH_FACTOR * velocityStrength;
            dimensions.z += WAVE_MODE_GROWTH_FACTOR * velocityStrength;
          

        }
      } else {
        if (dimensions.x >= SHRINK_LOWER_LIMIT) {
          dimensions.x -= SHRINK_FACTOR;
          dimensions.y -= SHRINK_FACTOR;
          dimensions.z -= SHRINK_FACTOR;
        }

      }

      //update the bubble to stay with the wand tip
      Entities.editEntity(_t.currentBubble, {
        position: _t.wandTipPosition,
        dimensions: dimensions
      });
    },
    spawnBubble: function() {
      var _t = this;
      //create a new bubble at the tip of the wand
      //the tip of the wand is going to be in a different place than the center, so we move in space relative to the model to find that position

      var properties = Entities.getEntityProperties(wandEntity.entityID);
      var wandPosition = properties.position;
      var upVector = Quat.getUp(properties.rotation);
      var frontVector = Quat.getFront(properties.rotation);
      var upOffset = Vec3.multiply(upVector, 0.2);
      var wandTipPosition = Vec3.sum(wandPosition, upOffset);
      _t.wandTipPosition = wandTipPosition;

      //store the position of the tip on spawn for use in velocity calculations
      _t.lastPosition = wandPosition;

      //create a bubble at the wand tip
      _t.currentBubble = Entities.addEntity({
        type: 'Model',
        modelURL: BUBBLE_MODEL,
        position: wandTipPosition,
        dimensions: {
          x: 0.01,
          y: 0.01,
          z: 0.01
        },
        collisionsWillMove: true, //true
        ignoreForCollisions: false, //false
        gravity: BUBBLE_GRAVITY,
        shapeType: "sphere",
        script: BUBBLE_SCRIPT,
      });
      //add this bubble to an array of bubbles so we can keep track of them
      _t.bubbles.push(_t.currentBubble)

    },
    returnToOriginalLocation: function() {
      var _t = this;
      _t.atOriginalLocation = true;
      Script.update.disconnect(BubbleWand.update)
      Entities.deleteEntity(_t.currentBubble);
      Entities.editEntity(wandEntity.entityID, _t.originalProperties)
      _t.spawnBubble();
      Script.update.connect(BubbleWand.update);

    },
    init: function() {
      var _t = this;
      this.spawnBubble();
      Script.update.connect(BubbleWand.update);

    }
  }



})