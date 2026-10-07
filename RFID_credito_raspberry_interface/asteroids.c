/* Asteroides - incluido por arcade.c (#include, como o display.c); nao compilar sozinho.
 * Usa do arcade.c: W/H/CX, controles (button_was_pressed...), melody_play, game_over, draw_header. */

typedef struct { float x, y, speed, radius; } Rock;
typedef struct Shoot {
    Vector2 position;
    Vector2 speed;
    float radius;
    float rotation;
    int lifeSpawn;
    bool active;
    Color color;
} Shoot;
typedef struct Player {
    Vector2 position;
    Vector2 speed;
    float acceleration;
    float rotation;
    Vector3 collider;
    Color color;
} Player;

/* Asteroides */
#define ROCKS 9
static Player player = { 0 };
static Rock rocks[ROCKS];
static int asteroid_score;
static int destroyedMeteorsCount;
static double asteroid_at;
#define PLAYER_MAX_SHOTS   5
static Shoot shots[PLAYER_MAX_SHOTS];
static const int screenWidth = 800;
static const int screenHeight = 450;
static float shipHeight = 0.0f;
#define PLAYER_SPEED        6.0f

static const Note ASTEROID_START[] = {{262,80},{392,80},{523,80},{392,80},{523,80},{784,220},{0,0}};
static const Note ASTEROID_OVER[]  = {{784,120},{659,120},{523,120},{415,120},{330,120},{220,120},{110,200},{0,0}};
static const Note SHOT_FIRE[]      = {{1568,25},{0,0}};
static const Note ROCK_BOOM[]      = {{110,90},{0,0}};

static void init_shot(Shoot *shot) {
    shot->position = (Vector2){0, 0}; shot->speed = (Vector2){0, 0};
    shot->radius = 2; shot->active = false; shot->lifeSpawn = 0; shot->color = WHITE;
}

static void start_asteroids(void) {
    destroyedMeteorsCount=0;
    asteroid_score=0;
    player.position.x = CX; asteroid_score = 0; asteroid_at = GetTime();
    
    player.position = (Vector2){screenWidth/2, screenHeight/2 - shipHeight/2};
    player.speed = (Vector2){0, 0};
    player.acceleration = 0;
    player.rotation = 0;
    player.collider = (Vector3){player.position.x, player.position.y - (shipHeight/2.5f), 12};
    player.color = LIGHTGRAY;
    
    for (int i = 0; i < ROCKS; i++) { rocks[i] = (Rock){GetRandomValue(25,W-25), GetRandomValue(-H,0), GetRandomValue(120,220), GetRandomValue(16,30)}; }
    for (int i = 0; i < PLAYER_MAX_SHOTS; i++) init_shot(&shots[i]);

}

/* "Foto" estatica para o carrossel do menu. */
static void preview_asteroids(Rectangle r) {
    static const int rk[][3] = {{126,98,23},{36,139,11},{205,139,11},{259,161,23},{179,197,11},{36,219,6},{222,207,6},{277,207,6},{345,208,11},{384,219,6}};
    DrawRectangleRec(r, (Color){12,14,29,255});
    for (int y = 10; y < r.height; y += 29) DrawCircle(r.x + (y*17)%(int)r.width, r.y + y, 1.5f, (Color){185,185,255,170});
    for (int i = 0; i < 10; i++) DrawCircle(r.x + rk[i][0], r.y + rk[i][1], rk[i][2], GRAY);
    DrawText("Pontos: 600", r.x + 10, r.y + 10, 14, YELLOW);
    int sx = r.x + 256;
    DrawTriangle((Vector2){sx, r.y + 232}, (Vector2){sx - 9, r.y + 254}, (Vector2){sx + 9, r.y + 254}, SKYBLUE);
}

static Vector2 player_rot(){
    return (Vector2) {sin(player.rotation*DEG2RAD),cos(player.rotation*DEG2RAD)};
}

static void init_rock(int i){
    rocks[i]=(Rock){GetRandomValue(25,W-25),GetRandomValue(-150,-20),rocks[i].speed+5,GetRandomValue(16,30)};
}

static void draw_asteroids(void) {
    float dt=GetFrameTime();
    if (joystick_is_held(LEFT)) player.position.x-=340*dt;
    if (joystick_is_held(RIGHT)) player.position.x+=340*dt;
    if (player.position.x<24) player.position.x=24;
    if (player.position.x>W-24) player.position.x=W-24;
    asteroid_score=(int)((GetTime()-asteroid_at)*10)+destroyedMeteorsCount*20;
    for (int i=0;i<ROCKS;i++) {
        rocks[i].y+=rocks[i].speed*dt;
        if (rocks[i].y>H+rocks[i].radius) init_rock(i);
        float x=rocks[i].x-player.position.x, y=rocks[i].y-(H-55), r=rocks[i].radius+18;
        if (x*x+y*y<r*r) { game_over("Asteroides", asteroid_score, ASTEROID_OVER); return; }
    }
    
    // Player shoot logic
    if (button_was_pressed(UP) || button_was_pressed(DOWN))
    {
        for (int i = 0; i < PLAYER_MAX_SHOTS; i++)
        {
            if (!shots[i].active)
            {
                Vector2 rot = player_rot();
                shots[i].position = (Vector2){ player.position.x + rot.x*(shipHeight), 440 };
                shots[i].active = true;
                shots[i].speed.x = 1.5*rot.x*PLAYER_SPEED;
                shots[i].speed.y = 1.5*rot.y*PLAYER_SPEED;
                shots[i].rotation = player.rotation;
                melody_play(SHOT_FIRE); /* substitui o clique da tecla */
                break;
            }
        }
    }
    
     // Shoot logic
    for (int i = 0; i < PLAYER_MAX_SHOTS; i++)
    {
        if (shots[i].active)
        {
            shots[i].lifeSpawn++;
            // Movement
            shots[i].position.x += shots[i].speed.x*dt*60;  /* speed era px/frame a 60 FPS */
            shots[i].position.y -= shots[i].speed.y*dt*60;

            // Collision logic: shoot vs walls
            if  ((shots[i].position.x > screenWidth + shots[i].radius) || (shots[i].position.x < 0 - shots[i].radius) || (shots[i].position.y > screenHeight + shots[i].radius) || (shots[i].position.y < 0 - shots[i].radius))
            {
                shots[i].active = false;
                shots[i].lifeSpawn = 0;
            }

            // Life of shoot
            if (shots[i].lifeSpawn >= 60) init_shot(&shots[i]);
            DrawCircleV(shots[i].position, shots[i].radius, shots[i].color);
        }
    }
        
    // Collision logic: player-shoots vs meteors
    for (int i = 0; i < PLAYER_MAX_SHOTS; i++)
    {
        if (shots[i].active)
        {
            for (int a = 0; a < ROCKS; a++)
            {
                if (CheckCollisionCircles(shots[i].position, shots[i].radius, (Vector2){rocks[a].x,rocks[a].y}, rocks[a].radius))
                {
                    shots[i].active = false;
                    shots[i].lifeSpawn = 0;
                    // bigMeteor[a].active = false;
                    destroyedMeteorsCount++;
                    melody_play(ROCK_BOOM);
                    
                    printf("%f", rocks[a].radius);
                    if (rocks[a].radius > 18) { rocks[a].radius=(int)rocks[a].radius*0.7; rocks[a].y+=20; }
                    else init_rock(a);

                    /* for (int j = 0; j < 2; j ++)
                    {
                        mediumMeteor[midMeteorsCount].position = (Vector2){bigMeteor[a].position.x, bigMeteor[a].position.y};
                       // mediumMeteor[midMeteorsCount].speed = (Vector2){cos(shots[i].rotation*DEG2RAD)*METEORS_SPEED*-1, sin(shots[i].rotation*DEG2RAD)*METEORS_SPEED*-1};

                        mediumMeteor[midMeteorsCount].active = true;
                        midMeteorsCount ++;
                    } */
                    //bigMeteor[a].position = (Vector2){-100, -100};
                    //rocks[a].color = RED;
                }
            }
        }
    }
    
    draw_header("ASTEROIDES", asteroid_score);
    DrawText("Joystick esquerda/direita", CX-115, 72, 18, LIGHTGRAY);
    for (int y=100;y<H;y+=43) DrawCircle((y*17)%W,y,1.5f,(Color){185,185,255,170});
    for (int i=0;i<ROCKS;i++) DrawCircleV((Vector2){rocks[i].x,rocks[i].y},rocks[i].radius,GRAY);
    DrawTriangle((Vector2){player.position.x,H-90},(Vector2){player.position.x-20,H-35},(Vector2){player.position.x+20,H-35},SKYBLUE);
}
