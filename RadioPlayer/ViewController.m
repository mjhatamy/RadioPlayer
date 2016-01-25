//
//  ViewController.m
//  RadioPlayer
//
//  Created by Majid Hatami Aghdam on 1/24/16.
//  Copyright © 2016 Majid Hatami Aghdam. All rights reserved.
//

#import "ViewController.h"

#include "ffplayer.h"

@interface ViewController ()

@end

@implementation ViewController{
   
}
#pragma mark Singleton Implementation
static ViewController *sharedObject;
+ (ViewController*)sharedInstance
{
    if (sharedObject == nil) {
        sharedObject = [[super allocWithZone:NULL] init];
    }
    return sharedObject;
}

-(instancetype)init{
    NSLog(@"init called here");
    if(self == [super init]){
    }
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    sharedObject = self;
    
    // Do any additional setup after loading the view, typically from a nib.
    
    

    ac = [AudioController sharedInstance];
  
    
}

- (void)didReceiveMemoryWarning {
    [super didReceiveMemoryWarning];
    // Dispose of any resources that can be recreated.
}


@end
