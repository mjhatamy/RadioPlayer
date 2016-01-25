//
//  ViewController.h
//  RadioPlayer
//
//  Created by Majid Hatami Aghdam on 1/24/16.
//  Copyright © 2016 Majid Hatami Aghdam. All rights reserved.
//

#import <UIKit/UIKit.h>

#import "AudioController.h"

@interface ViewController : UIViewController{
    AudioController *ac;
}

+ (ViewController*)sharedInstance;

@end

