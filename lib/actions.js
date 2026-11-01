// Independent gate locks permit commands to the other gate while one awaits confirmation.
export const actionResource=(op,args={})=>op.startsWith('gate.')?'gate:'+args.gate:'controller';
export class ActionLocks{
 constructor(){this.keys=new Set();}
 get busy(){return this.keys.size>0;}
 has(key){return this.keys.has(key);}
 acquire(key){if(this.has(key))return false;this.keys.add(key);return true;}
 release(key){this.keys.delete(key);}
}
